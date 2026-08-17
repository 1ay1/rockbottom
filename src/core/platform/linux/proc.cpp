// collectors/proc.cpp — walk /proc/<pid>/{stat,statm}, compute per-proc CPU%.

#include "../../sampler.hpp"
#include "procfs.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <unordered_map>
#include <unordered_set>

namespace rockbottom {

using namespace procfs;

void Sampler::sample_procs(Snapshot& snap, SortKey sort, int top_n, double dt) {
    DIR* proc = ::opendir("/proc");
    if (!proc) return;

    std::vector<ProcInfo> out;
    std::unordered_map<int, ProcPrev> cur;
    // The process count barely changes tick to tick, so size this tick's
    // containers from last tick's total (+headroom). Avoids the repeated
    // grow-and-rehash of push_back/insert over hundreds of pids on the
    // sampler thread the UI waits on. First tick prev_proc_ is empty — fall
    // back to a sane default so even the cold sample doesn't thrash.
    const std::size_t est = prev_proc_.empty() ? 512 : prev_proc_.size() + 32;
    out.reserve(est);
    cur.reserve(est);
    // Sum of every visible process's CPU% this tick. On Android/Termux, where
    // /proc/stat is sandbox-blocked, this is the ONLY way to know aggregate
    // load — we can still read our own uid's /proc/<pid>/stat.
    double proc_cpu_sum = 0;
    // pids seen this tick, used to prune the cmdline cache so it can't grow
    // unbounded as processes come and go over a long session.
    std::unordered_set<int> cmd_seen;
    cmd_seen.reserve(est);
    int total_procs = 0, total_threads = 0, running = 0, zombies = 0, dstate = 0;
    // CPU ticks a single core accrues over this interval. cpu% below is
    // reported as "% of one core" (100% = one full core saturated), matching
    // the darwin backend's 100*ns/(dt*1e9). Deliberately NOT multiplied by
    // ncpu_: a prior version scaled dt_ticks by ncpu_ here and then multiplied
    // the quotient by ncpu_ again below, so the two factors cancelled to this
    // exact value — a correct-looking no-op that a later "simplification" of
    // either site would turn into an N-times error. One honest factor now.
    double dt_ticks = dt * static_cast<double>(clk_tck_);
    // The pid whose expensive detail-only files (status, fd) we bother to read
    // this tick — the process the UI has open, or 0 for none. Loaded once.
    const int want_detail_pid = detail_pid_.load(std::memory_order_relaxed);

    // Boot epoch (seconds) = now - uptime; a process's start_sec is then
    // boot_epoch + starttime_ticks/CLK_TCK. Computed ONCE and cached: both
    // time() and uptime quantize to seconds, so recomputing per tick made the
    // result jitter ±1s and every process age wobble frame to frame.
    if (boot_epoch_ == 0)
        boot_epoch_ = static_cast<std::uint64_t>(std::time(nullptr)) - uptime_sec();
    const std::uint64_t boot_epoch = boot_epoch_;

    // Reusable scratch reused across EVERY pid this tick, so the per-process
    // reads don't churn the allocator. `path` is rebuilt in place (assign, no
    // fresh alloc after the first pid grows it); `fbuf` receives each /proc
    // file's bytes via slurp_into, which keeps the buffer's capacity. At 400
    // pids × 4 files/pid this replaces ~1600 string+stream allocations/tick
    // with none in steady state.
    std::string path;
    path.reserve(64);
    std::string fbuf;
    fbuf.reserve(4096);
    // Build "/proc/<pid>/<leaf>" into `path` without allocating; returns the
    // c_str for slurp_into / opendir.
    auto proc_path = [&](const char* pid_name, const char* leaf) -> const char* {
        path.assign("/proc/");
        path += pid_name;
        path += leaf;
        return path.c_str();
    };

    dirent* e;
    while ((e = ::readdir(proc)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        // Validated pid parse: reject overflow / trailing garbage (a malformed
        // /proc entry must not masquerade as pid 0). strtol over atoi.
        char* pend = nullptr;
        long pidl = std::strtol(e->d_name, &pend, 10);
        if (pend == e->d_name || *pend != '\0' || pidl <= 0 || pidl > INT_MAX) continue;
        int pid = static_cast<int>(pidl);

        if (!sys::slurp_into(proc_path(e->d_name, "/stat"), fbuf)) continue;
        const std::string& stat = fbuf;
        if (stat.empty()) continue;

        // comm sits in parens and may itself contain spaces/parens.
        auto lp = stat.find('('), rp = stat.rfind(')');
        if (lp == std::string::npos || rp == std::string::npos || rp < lp) continue;
        std::string comm = stat.substr(lp + 1, rp - lp - 1);

        // Pointer-walk the numeric tail (fields 3..22) instead of building an
        // istringstream: allocation-free, no locale/sentry overhead. `cur`
        // walks the space-separated tail after ")"; next_u64 skips leading
        // spaces then reads one unsigned integer, next_i64 a signed one.
        // `ok` trips false the moment a field is missing so a process that
        // died mid-read (truncated stat) is skipped rather than parsed into
        // garbage that would corrupt the CPU% delta.
        const char* c = stat.c_str() + rp + 1;
        bool ok = true;
        auto skip_ws = [&] { while (*c == ' ' || *c == '\t') ++c; };
        auto next_u64 = [&]() -> std::uint64_t {
            skip_ws();
            if (*c < '0' || *c > '9') { ok = false; return 0; }
            char* end = nullptr;
            std::uint64_t v = std::strtoull(c, &end, 10);
            if (end == c) { ok = false; return 0; }
            c = end; return v;
        };
        auto next_i64 = [&]() -> long long {
            skip_ws();
            if (*c != '-' && (*c < '0' || *c > '9')) { ok = false; return 0; }
            char* end = nullptr;
            long long v = std::strtoll(c, &end, 10);
            if (end == c) { ok = false; return 0; }
            c = end; return v;
        };
        auto skip_field = [&] { skip_ws(); while (*c && *c != ' ' && *c != '\t') ++c; };

        char state = '?';
        { skip_ws(); if (*c) state = *c++; else ok = false; }   // field 3
        int ppid = static_cast<int>(next_i64());                // field 4
        for (int i = 0; i < 5; ++i) skip_field();               // 5..9
        std::uint64_t minflt = next_u64(); (void)next_u64();    // 10 minflt, 11 cminflt
        std::uint64_t majflt = next_u64(); (void)next_u64();    // 12 majflt, 13 cmajflt
        std::uint64_t utime  = next_u64();                      // 14
        std::uint64_t stime  = next_u64();                      // 15
        skip_field(); skip_field();                             // 16 cutime, 17 cstime
        long prio = static_cast<long>(next_i64());              // 18 priority
        long nice = static_cast<long>(next_i64());              // 19 nice
        int threads = static_cast<int>(next_i64());             // 20
        skip_field();                                           // 21 itrealvalue
        std::uint64_t starttime = next_u64();                   // 22 starttime

        // A truncated stat (process exited between readdir and read) fails the
        // running parse — drop the row rather than emit a corrupt sample.
        if (!ok) continue;

        ++total_procs;
        total_threads += std::max(1, threads);
        if (state == 'R') ++running;
        if (state == 'Z') ++zombies;
        if (state == 'D') ++dstate;

        std::uint64_t cpu_ticks = utime + stime;
        // ONE lookup into last tick's state (was up to 5 hash probes/proc):
        // reuse this iterator for cpu, io, faults, csw and the history ring.
        auto prev_it = prev_proc_.find(pid);
        // Guard the delta cache against PID reuse: a recycled pid points at a
        // brand-new process whose cumulative counters are unrelated to the
        // dead one's. starttime (immutable per process) disambiguates them, so
        // a reused pid is treated as "no previous sample" (0 rates this tick)
        // instead of diffing against a stranger and emitting a phantom CPU/IO
        // spike that would sort to the top and can trip the verdict.
        const bool have_prev = !first_ && prev_it != prev_proc_.end()
                               && prev_it->second.starttime == starttime;
        ProcPrev& np = cur[pid];
        np.starttime = starttime;
        np.cpu_ticks = cpu_ticks;
        double cpu_pct = 0;
        if (have_prev && dt_ticks > 0) {
            std::uint64_t d = cpu_ticks > prev_it->second.cpu_ticks
                                  ? cpu_ticks - prev_it->second.cpu_ticks : 0;
            cpu_pct = 100.0 * static_cast<double>(d) / dt_ticks;
        }
        proc_cpu_sum += cpu_pct;

        // statm: total + resident pages (first two whitespace-separated ints).
        std::uint64_t rss_pages = 0, total_pages = 0;
        {
            // Reuse a SEPARATE small buffer: `stat` (== fbuf) is still needed
            // below for comm, so statm can't stomp it. statm is tiny.
            std::string sm = slurp(proc_path(e->d_name, "/statm"));
            const char* p = sm.c_str();
            char* end = nullptr;
            total_pages = std::strtoull(p, &end, 10);
            if (end != p) rss_pages = std::strtoull(end, nullptr, 10);
        }
        Bytes rss{rss_pages * static_cast<std::uint64_t>(page_size_)};

        // Per-process block-device I/O from /proc/<pid>/io. read_bytes /
        // write_bytes are cumulative bytes actually fetched from / sent to the
        // storage layer (not page-cache hits). Readable only for our own
        // processes unless privileged; unreadable rows just stay at 0.
        std::uint64_t io_r = 0, io_w = 0;
        {
            // stat/comm are fully parsed above, so fbuf is free to reuse here.
            sys::slurp_into(proc_path(e->d_name, "/io"), fbuf);
            const std::string& io = fbuf;
            // Scan lines for "read_bytes:" / "write_bytes:" without allocating
            // a stream: find the key, jump past the colon, parse the number.
            if (auto pos = io.find("read_bytes:"); pos != std::string::npos)
                io_r = std::strtoull(io.c_str() + pos + 11, nullptr, 10);
            if (auto pos = io.find("write_bytes:"); pos != std::string::npos)
                io_w = std::strtoull(io.c_str() + pos + 12, nullptr, 10);
        }
        ByteRate ior{}, iow{};
        if (have_prev && dt > 0) {
            const ProcPrev& pp = prev_it->second;
            if (io_r >= pp.io_read) ior.per_sec = static_cast<double>(io_r - pp.io_read) / dt;
            if (io_w >= pp.io_write) iow.per_sec = static_cast<double>(io_w - pp.io_write) / dt;
        }
        np.io_read = io_r;
        np.io_write = io_w;

        // Context switches from /proc/pid/status (voluntary + involuntary).
        // This is a LARGE file and its only consumer is the process detail
        // pane, so read it ONLY for the pid the UI is inspecting (see
        // set_detail_pid). Every other row keeps csw at 0 — the pane is the
        // sole reader and it's showing exactly this pid. Saves ~400 status
        // reads per tick in the common case (no pane open).
        std::uint64_t csw_total = 0;
        const bool want_detail = (pid == want_detail_pid);
        if (want_detail) {
            std::string st = slurp(proc_path(e->d_name, "/status"));
            if (auto pos = st.find("voluntary_ctxt_switches:"); pos != std::string::npos)
                csw_total += std::strtoull(st.c_str() + pos + 24, nullptr, 10);
            if (auto pos = st.find("nonvoluntary_ctxt_switches:"); pos != std::string::npos)
                csw_total += std::strtoull(st.c_str() + pos + 27, nullptr, 10);
        } else if (have_prev) {
            csw_total = prev_it->second.csw;   // carry forward so the rate stays 0, not a spike
        }

        // Page-fault + context-switch RATES: cumulative counters (majflt is
        // the honest "had to hit disk" fault; total faults = min+maj) diffed
        // across the tick. Stashed into ProcPrev so next sample can diff again.
        const std::uint64_t faults_total = minflt + majflt;
        double faults_ps = 0, csw_ps = 0;
        if (have_prev && dt > 0) {
            const ProcPrev& pp = prev_it->second;
            if (faults_total >= pp.faults)
                faults_ps = static_cast<double>(faults_total - pp.faults) / dt;
            if (csw_total >= pp.csw)
                csw_ps = static_cast<double>(csw_total - pp.csw) / dt;
        }
        np.faults = faults_total;
        np.csw = csw_total;

        // Rolling per-process cpu% ring — carry the prior history forward
        // (np was aggregate-reset above), push this interval's clamped
        // sample, so the detail pane can graph the process like htop's meter.
        if (have_prev) {
            np.cpu_hist = prev_it->second.cpu_hist;
            np.cpu_hist_len = prev_it->second.cpu_hist_len;
        }
        {
            auto& ring = np.cpu_hist;
            int& rl = np.cpu_hist_len;
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
        // The 48-float per-process history ring feeds ONE consumer: the detail
        // pane, for the selected pid. Copying it into every ProcInfo was
        // ~192 bytes × hundreds of procs of memcpy per tick, all discarded for
        // non-selected rows. The ring stays maintained in prev_proc_
        // (np.cpu_hist) for every pid, so selecting a process shows its full
        // history immediately; we just don't ship it out unless this is it.
        if (want_detail) {
            p.cpu_history = np.cpu_hist;
            p.hist_len = np.cpu_hist_len;
        }

        // Full command line from /proc/pid/cmdline: NUL-separated argv. argv
        // is IMMUTABLE after exec, so cache it and never re-read /proc for a
        // pid we've already seen. The cache key guards against pid RECYCLING:
        // a pid reused by a new process has a different starttime, so we mix
        // starttime into the stored value and re-read when it changes. Kernel
        // threads have an empty cmdline → fall back to [comm].
        {
            auto cit = cmd_cache_.find(pid);
            if (cit != cmd_cache_.end() && cit->second.first == starttime) {
                p.cmd = cit->second.second;
            } else {
                std::string raw = slurp(proc_path(e->d_name, "/cmdline"));
                if (!raw.empty()) {
                    for (char& ch : raw) if (ch == '\0') ch = ' ';
                    while (!raw.empty() && raw.back() == ' ') raw.pop_back();
                    p.cmd = std::move(raw);
                } else {
                    p.cmd = "[" + comm + "]";   // kernel thread
                }
                cmd_cache_[pid] = {starttime, p.cmd};
            }
            cmd_seen.insert(pid);
        }

        // Open file descriptors: count entries in /proc/pid/fd. This opendir+
        // readdir loop is expensive and, like status, feeds ONLY the detail
        // pane, so run it just for the inspected pid; other rows keep fds=-1
        // ("n/a"), which the pane already renders gracefully.
        if (want_detail) {
            if (DIR* fdd = ::opendir(proc_path(e->d_name, "/fd"))) {
                int nfd = 0;
                for (dirent* fe; (fe = ::readdir(fdd)) != nullptr; )
                    if (fe->d_name[0] != '.') ++nfd;
                ::closedir(fdd);
                p.fds = nfd;
            }
        }

        // Owner: resolve the process's uid, then map uid→name. Both steps are
        // cached. The uid is read via stat() on /proc/<pid>, but that syscall
        // is skipped entirely for a pid we've already seen (uid is fixed for
        // the life of a process) — guarded by starttime against pid reuse.
        {
            auto pit = puid_cache_.find(pid);
            unsigned uid;
            if (pit != puid_cache_.end() && pit->second.first == starttime) {
                uid = pit->second.second;
            } else {
                struct ::stat stbuf{};
                uid = (::stat(proc_path(e->d_name, ""), &stbuf) == 0)
                          ? static_cast<unsigned>(stbuf.st_uid) : 0;
                puid_cache_[pid] = {starttime, uid};
            }
            auto uit = uid_cache_.find(uid);
            if (uit == uid_cache_.end())
                uit = uid_cache_.emplace(uid, user_of(static_cast<uid_t>(uid))).first;
            p.user = uit->second;
        }

        if (auto it = pid_ports_.find(pid); it != pid_ports_.end())
            p.ports = it->second;

        out.push_back(std::move(p));
    }
    ::closedir(proc);

    prev_proc_ = std::move(cur);
    // Prune the per-pid caches to pids still alive this tick (bounded memory
    // over a long-running session where pids churn).
    if (cmd_cache_.size() > cmd_seen.size() * 2 + 64) {
        for (auto it = cmd_cache_.begin(); it != cmd_cache_.end(); )
            it = cmd_seen.count(it->first) ? std::next(it) : cmd_cache_.erase(it);
        for (auto it = puid_cache_.begin(); it != puid_cache_.end(); )
            it = cmd_seen.count(it->first) ? std::next(it) : puid_cache_.erase(it);
    }
    snap.proc_count = total_procs;
    snap.thread_count = total_threads;
    snap.running = running;
    snap.zombies = zombies;
    snap.dstate = dstate;

    // ── Android/Termux degraded-CPU fallback ──
    // /proc/stat is sandbox-blocked, so sample_cpu left cpu.total at 0 and
    // didn't advance the history ring. Reconstruct aggregate utilisation from
    // the summed per-process CPU% (proc_cpu_sum is in "% of one core" units;
    // divide by core count for a 0..1 machine-wide fraction). We only see our
    // own uid's processes, so this is a floor, not the whole machine — but it
    // tracks *our* load faithfully, which is what a Termux user actually cares
    // about, and it makes the flagship graph live instead of a dead flat line.
    if (!cpu_stat_ok_) {
        double frac = ncpu_ > 0 ? (proc_cpu_sum / 100.0) / static_cast<double>(ncpu_) : 0.0;
        snap.cpu.total = Ratio{frac};
        push_hist(total_hist_, total_hist_len_, static_cast<float>(frac));
        snap.cpu.total_history = total_hist_;
        snap.cpu.total_hist_len = total_hist_len_;
    }
    // /proc/loadavg is likewise blocked; sysinfo(2) exposes the kernel's real
    // 1/5/15 load averages without a procfs node, so use that. (It's the same
    // three numbers /proc/loadavg formats, scaled by 1<<SI_LOAD_SHIFT.)
    if (!loadavg_ok_) {
        struct ::sysinfo si{};
        if (::sysinfo(&si) == 0) {
            constexpr double kScale = 1.0 / static_cast<double>(1 << SI_LOAD_SHIFT);
            snap.cpu.loadavg = {static_cast<double>(si.loads[0]) * kScale,
                                static_cast<double>(si.loads[1]) * kScale,
                                static_cast<double>(si.loads[2]) * kScale};
        }
    }

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
