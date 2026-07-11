// collectors/proc.cpp — walk /proc/<pid>/{stat,statm}, compute per-proc CPU%.

#include "../../sampler.hpp"
#include "procfs.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>

namespace rockbottom {

using namespace procfs;

void Sampler::sample_procs(Snapshot& snap, SortKey sort, int top_n, double dt) {
    DIR* proc = ::opendir("/proc");
    if (!proc) return;

    std::vector<ProcInfo> out;
    std::unordered_map<int, ProcPrev> cur;
    int total_procs = 0, total_threads = 0, running = 0, zombies = 0, dstate = 0;
    double dt_ticks = dt * static_cast<double>(clk_tck_) * static_cast<double>(ncpu_);

    // Boot epoch (seconds) = now - uptime; a process's start_sec is then
    // boot_epoch + starttime_ticks/CLK_TCK. Computed once per sample.
    const std::uint64_t boot_epoch =
        static_cast<std::uint64_t>(std::time(nullptr)) - uptime_sec();

    dirent* e;
    while ((e = ::readdir(proc)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = std::atoi(e->d_name);
        std::string base = "/proc/" + std::string(e->d_name);

        std::string stat = slurp(base + "/stat");
        if (stat.empty()) continue;

        // comm sits in parens and may itself contain spaces/parens.
        auto lp = stat.find('('), rp = stat.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos) continue;
        std::string comm = stat.substr(lp + 1, rp - lp - 1);
        std::istringstream ss(stat.substr(rp + 2));
        char state = '?';
        ss >> state;                                // field 3
        int ppid = 0; ss >> ppid;                   // field 4
        std::string skip;
        // fields 5..9: pgrp session tty_nr tpgid flags
        for (int i = 0; i < 5; ++i) ss >> skip;
        std::uint64_t minflt = 0, cminflt = 0, majflt = 0, cmajflt = 0;
        ss >> minflt >> cminflt >> majflt >> cmajflt;   // 10 11 12 13
        std::uint64_t utime = 0, stime = 0;
        ss >> utime >> stime;                        // 14, 15
        ss >> skip >> skip;                          // 16, 17 (cutime, cstime)
        long prio = 0, nice = 0;
        ss >> prio >> nice;                          // 18 priority, 19 nice
        int threads = 0; ss >> threads;              // 20
        ss >> skip;                                  // 21 itrealvalue
        std::uint64_t starttime = 0; ss >> starttime;// 22 starttime (ticks since boot)

        ++total_procs;
        total_threads += std::max(1, threads);
        if (state == 'R') ++running;
        if (state == 'Z') ++zombies;
        if (state == 'D') ++dstate;

        std::uint64_t cpu_ticks = utime + stime;
        cur[pid] = {cpu_ticks};
        double cpu_pct = 0;
        if (!first_ && prev_proc_.count(pid) && dt_ticks > 0) {
            std::uint64_t d = cpu_ticks > prev_proc_[pid].cpu_ticks
                                  ? cpu_ticks - prev_proc_[pid].cpu_ticks : 0;
            cpu_pct = 100.0 * static_cast<double>(d) / dt_ticks * static_cast<double>(ncpu_);
        }

        std::uint64_t rss_pages = 0, total_pages = 0;
        { std::ifstream sm(base + "/statm"); sm >> total_pages >> rss_pages; }
        Bytes rss{rss_pages * static_cast<std::uint64_t>(page_size_)};

        // Per-process block-device I/O from /proc/<pid>/io. read_bytes /
        // write_bytes are cumulative bytes actually fetched from / sent to the
        // storage layer (not page-cache hits). Readable only for our own
        // processes unless privileged; unreadable rows just stay at 0.
        std::uint64_t io_r = 0, io_w = 0;
        {
            std::ifstream iof(base + "/io");
            std::string key;
            std::uint64_t val;
            while (iof >> key >> val) {
                if (key == "read_bytes:")  io_r = val;
                else if (key == "write_bytes:") io_w = val;
            }
        }
        ByteRate ior{}, iow{};
        if (!first_ && prev_proc_.count(pid) && dt > 0) {
            const auto& pp = prev_proc_[pid];
            if (io_r >= pp.io_read) ior.per_sec = static_cast<double>(io_r - pp.io_read) / dt;
            if (io_w >= pp.io_write) iow.per_sec = static_cast<double>(io_w - pp.io_write) / dt;
        }
        cur[pid].io_read = io_r;
        cur[pid].io_write = io_w;

        // Context switches from /proc/pid/status (voluntary + involuntary).
        std::uint64_t csw_total = 0;
        {
            std::ifstream stf(base + "/status");
            std::string line;
            while (std::getline(stf, line)) {
                if (line.rfind("voluntary_ctxt_switches:", 0) == 0 ||
                    line.rfind("nonvoluntary_ctxt_switches:", 0) == 0) {
                    csw_total += std::strtoull(line.c_str() + line.find(':') + 1, nullptr, 10);
                }
            }
        }

        // Page-fault + context-switch RATES: cumulative counters (majflt is
        // the honest "had to hit disk" fault; total faults = min+maj) diffed
        // across the tick. Stashed into ProcPrev so next sample can diff again.
        const std::uint64_t faults_total = minflt + majflt;
        double faults_ps = 0, csw_ps = 0;
        if (!first_ && prev_proc_.count(pid) && dt > 0) {
            const auto& pp = prev_proc_[pid];
            if (faults_total >= pp.faults)
                faults_ps = static_cast<double>(faults_total - pp.faults) / dt;
            if (csw_total >= pp.csw)
                csw_ps = static_cast<double>(csw_total - pp.csw) / dt;
        }
        cur[pid].faults = faults_total;
        cur[pid].csw = csw_total;

        // Rolling per-process cpu% ring — carry the prior history forward
        // (cur[pid] was aggregate-reset above), push this interval's clamped
        // sample, so the detail pane can graph the process like htop's meter.
        if (auto pit = prev_proc_.find(pid); pit != prev_proc_.end()) {
            cur[pid].cpu_hist = pit->second.cpu_hist;
            cur[pid].cpu_hist_len = pit->second.cpu_hist_len;
        }
        {
            auto& ring = cur[pid].cpu_hist;
            int& rl = cur[pid].cpu_hist_len;
            const float sample = static_cast<float>(std::clamp(cpu_pct / 100.0, 0.0, 1.0));
            if (rl < static_cast<int>(ring.size())) {
                ring[static_cast<std::size_t>(rl++)] = sample;
            } else {
                std::move(ring.begin() + 1, ring.end(), ring.begin());
                ring.back() = sample;
            }
        }

        ProcInfo p;
        p.pid = pid;
        p.ppid = ppid;
        p.name = comm;
        p.state = state;
        p.threads = std::max(1, threads);
        p.cpu = cpu_pct;
        p.prio = static_cast<int>(prio);
        p.nice = static_cast<int>(nice);
        p.cpu_ms = (utime + stime) * 1000ULL / static_cast<std::uint64_t>(clk_tck_);
        if (starttime > 0)
            p.start_sec = boot_epoch + starttime / static_cast<std::uint64_t>(clk_tck_);
        p.rss = rss;
        p.virt = Bytes{total_pages * static_cast<std::uint64_t>(page_size_)};
        p.mem_share = Ratio::of(rss, ram_total_);
        p.io_read = ior;
        p.io_write = iow;
        p.faults_ps = faults_ps;
        p.csw_ps = csw_ps;
        p.pageins = majflt;   // lifetime major faults = blocking disk pageins
        p.cpu_history = cur[pid].cpu_hist;
        p.hist_len = cur[pid].cpu_hist_len;

        // Full command line from /proc/pid/cmdline: NUL-separated argv. We keep
        // the whole thing (NULs → spaces) so the table can distinguish two
        // `python` rows by their script, and the detail pane shows the invocation
        // verbatim. Kernel threads have an empty cmdline → fall back to [comm].
        {
            std::string raw = slurp(base + "/cmdline");
            if (!raw.empty()) {
                for (char& c : raw) if (c == '\0') c = ' ';
                while (!raw.empty() && raw.back() == ' ') raw.pop_back();
                p.cmd = std::move(raw);
            } else {
                p.cmd = "[" + comm + "]";   // kernel thread
            }
        }

        // Open file descriptors: count entries in /proc/pid/fd (readable for our
        // own procs, or all when privileged; unreadable rows stay at -1).
        if (DIR* fdd = ::opendir((base + "/fd").c_str())) {
            int nfd = 0;
            for (dirent* fe; (fe = ::readdir(fdd)) != nullptr; )
                if (fe->d_name[0] != '.') ++nfd;
            ::closedir(fdd);
            p.fds = nfd;
        }

        struct ::stat stbuf{};
        if (::stat(base.c_str(), &stbuf) == 0) p.user = user_of(stbuf.st_uid);

        if (auto it = pid_ports_.find(pid); it != pid_ports_.end())
            p.ports = it->second;

        out.push_back(std::move(p));
    }
    ::closedir(proc);

    prev_proc_ = std::move(cur);
    snap.proc_count = total_procs;
    snap.thread_count = total_threads;
    snap.running = running;
    snap.zombies = zombies;
    snap.dstate = dstate;

    using Cmp = bool (*)(const ProcInfo&, const ProcInfo&);
    auto by = [](SortKey k) -> Cmp {
        switch (k) {
            case SortKey::Cpu:  return [](const ProcInfo& a, const ProcInfo& b){ return a.cpu > b.cpu; };
            case SortKey::Mem:  return [](const ProcInfo& a, const ProcInfo& b){ return a.rss.value > b.rss.value; };
            case SortKey::Io:   return [](const ProcInfo& a, const ProcInfo& b){
                    return (a.io_read.per_sec + a.io_write.per_sec)
                         > (b.io_read.per_sec + b.io_write.per_sec); };
            case SortKey::Pid:  return [](const ProcInfo& a, const ProcInfo& b){ return a.pid < b.pid; };
            case SortKey::Name: return [](const ProcInfo& a, const ProcInfo& b){ return a.name < b.name; };
            case SortKey::Port:
                // Processes with bound ports first, ascending by lowest port;
                // ties (and the portless tail) fall back to CPU.
                return [](const ProcInfo& a, const ProcInfo& b){
                    const bool ha = !a.ports.empty(), hb = !b.ports.empty();
                    if (ha != hb) return ha;
                    if (ha && a.ports.front() != b.ports.front())
                        return a.ports.front() < b.ports.front();
                    return a.cpu > b.cpu;
                };
        }
        return [](const ProcInfo& a, const ProcInfo& b){ return a.cpu > b.cpu; };
    };
    std::sort(out.begin(), out.end(), by(sort));
    // top_n <= 0 means "keep everything" — the UI windows/scrolls and tree mode
    // needs full parentage, so we no longer drop processes here. A positive cap
    // still trims (kept for callers that want a bounded snapshot).
    if (top_n > 0 && static_cast<int>(out.size()) > top_n)
        out.resize(static_cast<std::size_t>(top_n));
    snap.procs = std::move(out);
}

}  // namespace rockbottom
