// collectors/proc.cpp — walk /proc/<pid>/{stat,statm}, compute per-proc CPU%.

#include "../sampler.hpp"
#include "../procfs.hpp"

#include <algorithm>
#include <cstdlib>
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
        ss >> state;
        std::string skip;
        for (int i = 0; i < 10; ++i) ss >> skip;   // fields 4..13
        std::uint64_t utime = 0, stime = 0;
        ss >> utime >> stime;                       // 14, 15
        for (int i = 0; i < 4; ++i) ss >> skip;     // 16..19
        int threads = 0; ss >> threads;             // 20

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

        ProcInfo p;
        p.pid = pid;
        p.name = comm;
        p.state = state;
        p.threads = std::max(1, threads);
        p.cpu = cpu_pct;
        p.rss = rss;
        p.mem_share = Ratio::of(rss, ram_total_);
        p.io_read = ior;
        p.io_write = iow;

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
    if (static_cast<int>(out.size()) > top_n) out.resize(static_cast<std::size_t>(top_n));
    snap.procs = std::move(out);
}

}  // namespace rockbottom
