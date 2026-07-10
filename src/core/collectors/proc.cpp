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

namespace bottom {

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

        ProcInfo p;
        p.pid = pid;
        p.name = comm;
        p.state = state;
        p.threads = std::max(1, threads);
        p.cpu = cpu_pct;
        p.rss = rss;
        p.mem_share = Ratio::of(rss, ram_total_);

        struct ::stat stbuf{};
        if (::stat(base.c_str(), &stbuf) == 0) p.user = user_of(stbuf.st_uid);

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
            case SortKey::Pid:  return [](const ProcInfo& a, const ProcInfo& b){ return a.pid < b.pid; };
            case SortKey::Name: return [](const ProcInfo& a, const ProcInfo& b){ return a.name < b.name; };
        }
        return [](const ProcInfo& a, const ProcInfo& b){ return a.cpu > b.cpu; };
    };
    std::sort(out.begin(), out.end(), by(sort));
    if (static_cast<int>(out.size()) > top_n) out.resize(static_cast<std::size_t>(top_n));
    snap.procs = std::move(out);
}

}  // namespace bottom
