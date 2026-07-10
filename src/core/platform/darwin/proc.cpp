// platform/darwin/proc.cpp — libproc process walk + per-proc CPU / mem / I/O.
//
// The macOS analogue of walking /proc/<pid>. proc_listpids() enumerates every
// pid; proc_pidinfo(PROC_PIDTASKALLINFO) yields the BSD info (name, uid, run
// state, thread count) and the task info (cumulative user+system CPU time in
// MACH ABSOLUTE-TIME ticks, resident size). Those ticks are NOT nanoseconds on
// Apple Silicon (timebase 125/3) — we convert them with mach_timebase_info and
// delta the result across the tick, the direct analogue of deltaing
// utime+stime jiffies on Linux. Per-process disk bytes come from
// proc_pid_rusage(RUSAGE_INFO_V2), the honest storage-layer counters Activity
// Monitor uses. Bound ports are joined in from sample_ports() as on Linux.

#include "../../sampler.hpp"
#include "mach_util.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/proc_info.h>
#include <sys/resource.h>

namespace rockbottom {

namespace {

// Nanoseconds per mach absolute-time tick (numer/denom), read once. On Intel
// Macs this is 1.0 (ticks already ns); on Apple Silicon it's ~41.67.
double mach_ns_per_tick() {
    static const double f = [] {
        mach_timebase_info_data_t tb{};
        if (::mach_timebase_info(&tb) != KERN_SUCCESS || tb.denom == 0) return 1.0;
        return static_cast<double>(tb.numer) / static_cast<double>(tb.denom);
    }();
    return f;
}

}  // namespace

void Sampler::sample_procs(Snapshot& snap, SortKey sort, int top_n, double dt) {
    int cap = ::proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (cap <= 0) return;
    std::vector<pid_t> pids(static_cast<std::size_t>(cap) / sizeof(pid_t) + 16);
    int got = ::proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                              static_cast<int>(pids.size() * sizeof(pid_t)));
    if (got <= 0) return;
    int npids = got / static_cast<int>(sizeof(pid_t));

    std::vector<ProcInfo> out;
    std::unordered_map<int, ProcPrev> cur;
    int total_procs = 0, total_threads = 0, running = 0, zombies = 0, dstate = 0;

    for (int i = 0; i < npids; ++i) {
        int pid = pids[static_cast<std::size_t>(i)];
        if (pid <= 0) continue;

        proc_taskallinfo tai{};
        if (::proc_pidinfo(pid, PROC_PIDTASKALLINFO, 0, &tai, sizeof tai) < static_cast<int>(sizeof tai))
            continue;   // vanished or not permitted — skip like an unreadable /proc row

        int threads = static_cast<int>(tai.ptinfo.pti_threadnum);
        ++total_procs;
        total_threads += std::max(1, threads);

        // Cumulative CPU time in mach ticks → nanoseconds via the timebase.
        std::uint64_t cpu_ns = static_cast<std::uint64_t>(
            static_cast<double>(tai.ptinfo.pti_total_user + tai.ptinfo.pti_total_system) *
            mach_ns_per_tick());
        cur[pid].cpu_ticks = cpu_ns;
        double cpu_pct = 0;
        if (!first_ && prev_proc_.count(pid) && dt > 0) {
            std::uint64_t d = cpu_ns > prev_proc_[pid].cpu_ticks
                                  ? cpu_ns - prev_proc_[pid].cpu_ticks : 0;
            // ns busy / ns elapsed → fraction of one core, ×100 for percent
            // (matches Linux's per-core convention: can exceed 100 across cores).
            cpu_pct = 100.0 * static_cast<double>(d) / (dt * 1e9);
        }

        // Run state. macOS's pbsd.pbi_status is the *process* status (nearly
        // always SRUN) — useless as Linux's per-thread R/S/D. So we do what the
        // UI actually means by "running": a process burning CPU this interval is
        // R (● working), otherwise S (sleeping). Zombies come from SZOMB. There
        // is no uninterruptible-D census on macOS, so dstate stays 0.
        char state;
        if (tai.pbsd.pbi_status == SZOMB)  state = 'Z';
        else if (cpu_pct > 0.5)            state = 'R';
        else                              state = 'S';
        if (state == 'R') ++running;
        if (state == 'Z') ++zombies;

        Bytes rss{tai.ptinfo.pti_resident_size};

        // Honest per-process disk bytes (not cache hits) via rusage_info_v2.
        // RUSAGE_INFO_V4 additionally carries phys_footprint — the figure
        // Activity Monitor's "Memory" column shows — plus lifetime pageins.
        std::uint64_t io_r = 0, io_w = 0, footprint = 0, pageins = 0;
        rusage_info_v4 ru{};
        if (::proc_pid_rusage(pid, RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ru)) == 0) {
            io_r = ru.ri_diskio_bytesread;
            io_w = ru.ri_diskio_byteswritten;
            footprint = ru.ri_phys_footprint;
            pageins = ru.ri_pageins;
        }
        ByteRate ior{}, iow{};
        if (!first_ && prev_proc_.count(pid) && dt > 0) {
            const auto& pp = prev_proc_[pid];
            if (io_r >= pp.io_read)  ior.per_sec = static_cast<double>(io_r - pp.io_read)  / dt;
            if (io_w >= pp.io_write) iow.per_sec = static_cast<double>(io_w - pp.io_write) / dt;
        }
        cur[pid].io_read = io_r;
        cur[pid].io_write = io_w;

        // Page-fault + context-switch RATES from the cumulative counters in
        // the task info — same delta discipline as CPU time.
        const std::uint64_t faults = tai.ptinfo.pti_faults;
        const std::uint64_t csw    = tai.ptinfo.pti_csw;
        double faults_ps = 0, csw_ps = 0;
        if (!first_ && prev_proc_.count(pid) && dt > 0) {
            const auto& pp = prev_proc_[pid];
            if (faults >= pp.faults) faults_ps = static_cast<double>(faults - pp.faults) / dt;
            if (csw >= pp.csw)       csw_ps    = static_cast<double>(csw - pp.csw) / dt;
        }
        cur[pid].faults = faults;
        cur[pid].csw = csw;

        ProcInfo p;
        p.pid = pid;
        p.ppid = static_cast<int>(tai.pbsd.pbi_ppid);
        p.name = tai.pbsd.pbi_name[0] ? tai.pbsd.pbi_name : tai.pbsd.pbi_comm;
        p.state = state;
        p.threads = std::max(1, threads);
        p.prio = tai.ptinfo.pti_priority;
        p.nice = tai.pbsd.pbi_nice;
        p.cpu = cpu_pct;
        p.cpu_ms = cpu_ns / 1000000;
        p.start_sec = tai.pbsd.pbi_start_tvsec;
        p.rss = rss;
        p.footprint = Bytes{footprint};
        p.mem_share = Ratio::of(rss, ram_total_);
        p.faults_ps = faults_ps;
        p.csw_ps = csw_ps;
        p.pageins = pageins;
        p.io_read = ior;
        p.io_write = iow;
        p.user = sys::user_of(tai.pbsd.pbi_uid);

        // Open-fd census — proc_pidinfo(LISTFDS) with a null buffer returns
        // the byte size needed; divide by the record size for the count.
        int fdbytes = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        p.fds = fdbytes > 0 ? fdbytes / PROC_PIDLISTFD_SIZE : -1;

        // Full command-line path for the process-detail pane, if readable.
        char pathbuf[PROC_PIDPATHINFO_MAXSIZE] = {};
        if (::proc_pidpath(pid, pathbuf, sizeof pathbuf) > 0) p.cmd = pathbuf;

        if (auto it = pid_ports_.find(pid); it != pid_ports_.end())
            p.ports = it->second;

        out.push_back(std::move(p));
    }

    prev_proc_ = std::move(cur);
    snap.proc_count = total_procs;
    snap.thread_count = total_threads;
    snap.running = running;
    snap.zombies = zombies;
    snap.dstate = dstate;   // no uninterruptible-wait census on macOS; stays 0

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
