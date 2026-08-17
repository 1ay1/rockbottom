// sampler.cpp — Orchestration only. Per-domain collection lives in
// collectors/*.cpp; this file wires them together and owns lifecycle.

#include "sampler.hpp"
#include "core_temps.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <thread>
#include <sys/resource.h>
#include <unistd.h>

namespace rockbottom {

// signal_process is pure POSIX (kill(2)) — identical on Linux and macOS — so it
// lives in the OS-agnostic orchestrator rather than a per-platform backend.
std::string signal_process(int pid, int sig) {
    // Reject non-positive pids: kill(0,...) hits our OWN process group,
    // kill(-1,...) every process we may signal, kill(-pgid,...) a whole
    // group. A corrupted selection must never fan a signal out that way —
    // this UI only ever targets one real, listed process.
    if (pid <= 0) return "invalid process id";
    if (::kill(pid, sig) == 0) return {};
    switch (errno) {
        case EPERM: return "permission denied — not your process";
        case ESRCH: return "process no longer exists";
        default:    return "kill failed";
    }
}

// renice_process is also pure POSIX (setpriority(2)). Lowering a nice value
// (raising priority) requires privilege; raising it (yielding CPU) is always
// allowed for your own processes.
std::string renice_process(int pid, int nice) {
    if (pid <= 0) return "invalid process id";
    if (nice < -20) nice = -20;
    if (nice > 19)  nice = 19;
    errno = 0;
    if (::setpriority(PRIO_PROCESS, static_cast<id_t>(pid), nice) == 0) return {};
    switch (errno) {
        case EPERM:
        case EACCES: return "permission denied — raising priority needs privilege";
        case ESRCH:  return "process no longer exists";
        default:     return "renice failed";
    }
}

// read_static() and every sample_*() collector are defined per-platform under
// platform/<os>/. sysconf() is POSIX, so the tick-rate / page-size probe is
// safe to keep here in the OS-agnostic orchestrator; the constructor then hands
// off to the platform read_static() for the machine-identity facts.
Sampler::Sampler() {
    clk_tck_ = ::sysconf(_SC_CLK_TCK);
    if (clk_tck_ <= 0) clk_tck_ = 100;
    page_size_ = ::sysconf(_SC_PAGESIZE);
    if (page_size_ <= 0) page_size_ = 4096;
    read_static();
}

// Stamp every logical core with the class + physical id discovered once at
// read_static(), then recompute the cluster tallies from what was actually
// stamped. Deriving the counts here (instead of trusting a separate sysctl)
// keeps CpuCore::kind and CpuInfo::perf_cores structurally incapable of
// disagreeing — the histogram IS the counts.
void Sampler::apply_topology(CpuInfo& cpu) const {
    cpu.perf_cores = cpu.eff_cores = 0;
    cpu.perf_label = perf_label_;
    cpu.eff_label  = eff_label_;
    for (std::size_t i = 0; i < cpu.cores.size(); ++i) {
        CpuCore& c = cpu.cores[i];
        c.kind = i < core_kind_.size() ? core_kind_[i] : CoreKind::Unknown;
        c.phys = i < core_phys_.size() ? core_phys_[i] : -1;
        if (c.kind == CoreKind::Perf) ++cpu.perf_cores;
        else if (c.kind == CoreKind::Eff) ++cpu.eff_cores;
    }
    // A "heterogeneous" machine where one cluster is empty is just a
    // homogeneous machine with a noisy probe — fall back to the flat view
    // rather than labelling every core P.
    if (cpu.perf_cores == 0 || cpu.eff_cores == 0) {
        cpu.perf_cores = cpu.eff_cores = 0;
        cpu.perf_label.clear();
        cpu.eff_label.clear();
        for (CpuCore& c : cpu.cores) c.kind = CoreKind::Unknown;
    }
}

// Attach per-core die temperatures to the cores themselves.
//
// The resolution rules live in core_temps.hpp as a pure function; this is just
// the member that hands it the sampler's cached sibling map. Doing the work
// HERE rather than in the widget is what stopped the UI from parsing sensor
// labels — every view now sees the same resolved figure.
void Sampler::apply_core_temps(CpuInfo& cpu, const std::vector<Sensor>& sensors) const {
    resolve_core_temps(cpu, sensors, core_id_siblings_);
}

Snapshot Sampler::sample(SortKey sort, int top_n, bool fast) {
    auto now = std::chrono::steady_clock::now();
    double dt = first_ ? 0.0 : std::chrono::duration<double>(now - last_time_).count();
    if (dt <= 0 && !first_) dt = 0.001;
    last_time_ = now;

    // Is a throttled collector due? True on first sample, or once `period` has
    // elapsed since it last ran. Updates the stamp in place when it fires, so
    // the cadence is wall-clock and INDEPENDENT of the refresh interval: at
    // 250ms refresh a 3s collector still runs once per 3s, not 12× more often.
    auto due = [&](std::chrono::steady_clock::time_point& at,
                   std::chrono::milliseconds period) -> bool {
        if (first_ || at.time_since_epoch().count() == 0 || now - at >= period) {
            at = now;
            return true;
        }
        return false;
    };
    using ms = std::chrono::milliseconds;

    Snapshot s;
    s.hostname = hostname_;
    s.kernel = kernel_;
    s.uptime_sec = uptime_sec();

    // Env-gated phase timing (RB_PHASE=1): prints per-collector ms to stderr.
    // Debug aid only; zero cost when the var is unset.
    static const bool kPhase = [] { const char* e = std::getenv("RB_PHASE"); return e && *e && *e != '0'; }();
    auto pnow = [] { return std::chrono::steady_clock::now(); };
    auto pms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    auto pt = pnow();
    auto phase = [&](const char* nm) { if (kPhase) { auto n = pnow(); std::fprintf(stderr, "  %-14s %.3f ms\n", nm, pms(pt, n)); pt = n; } };

    // Fast, genuinely per-tick metrics: CPU, memory, network, disk I/O rates,
    // GPU. These change every frame and must run every tick.
    sample_cpu(s.cpu);        phase("cpu");
    sample_mem(s.mem);        phase("mem");
    sample_mem_rates(s.mem, dt);
    sample_disk_io(s.disk_io, dt); phase("disk_io");
    sample_drives(s.drives, dt);   phase("drives");
    sample_net(s.nets, dt);   phase("net");
    // GPU collection can fork nvidia-smi, the most expensive thing the sampler
    // does. Two cadences: the always-visible gauges (util/temp/vram/clocks)
    // refresh every ~1s, but the per-process app table — which needs TWO more
    // forks (`--query-compute-apps` + `pmon -c 1`) or an O(all-pids) fdinfo
    // walk — refreshes every ~3s and is carried over in between. The fast
    // startup prime skips GPU entirely, so the first paint never blocks.
    //
    // GPU is independent of every other collector and NVIDIA's subprocess can
    // take 25-150ms. Run it concurrently so process, port, sensor, disk and PSI
    // work is hidden under that latency instead of added to it. There is never
    // more than one sample in flight for a Sampler; the worker owns a private
    // output snapshot and its GPU-specific delta cache until we join below.
    // Build into a private snapshot and publish only after a successful join,
    // so an exception cannot leave the visible cache half-cleared. Declare all
    // captured state before the worker: reverse destruction then joins the
    // jthread before destroying its references if another collector throws.
    std::vector<GpuInfo> gpu_next;
    std::exception_ptr gpu_error;
    std::optional<std::jthread> gpu_worker;
    std::chrono::steady_clock::time_point gpu_started{};
    if (!fast && due(gpus_at_, ms(1000))) {
        const bool with_procs = due(gpu_procs_at_, ms(3000));
        gpu_started = pnow();
        gpu_next = gpus_cache_;  // carries the slower per-process table forward
        gpu_worker.emplace([this, with_procs, &gpu_next, &gpu_error] {
            try {
                sample_gpu(gpu_next, with_procs);
            } catch (...) {
                gpu_error = std::current_exception();
            }
        });
    }

    // Disk CAPACITY (statvfs per mount) barely moves — refresh ~every 5s.
    if (due(disks_at_, ms(5000))) { disks_cache_.clear(); sample_disks(disks_cache_); }
    s.disks = disks_cache_;
    phase("disks");

    // Hardware temperatures drift slowly — refresh ~every 2s.
    if (due(sensors_at_, ms(2000))) { sensors_cache_.clear(); sample_sensors(sensors_cache_); }
    s.sensors = sensors_cache_;
    phase("sensors");

    // SSD write-endurance (NVMe SMART ioctl) is a glacial figure — it moves
    // percentage points over MONTHS — and the ioctl needs privilege, so it's
    // refreshed only ~every 30s and cached. On non-root / non-NVMe the
    // collector leaves the vector empty and the UI shows nothing. Skipped on a
    // fast startup prime.
    if (!fast && due(ssd_at_, ms(30000))) { ssd_cache_.clear(); sample_ssd_health(ssd_cache_); }
    s.ssd_health = ssd_cache_;
    phase("ssd");

    // Resolve per-core die temps onto the cores now that both halves exist.
    // Doing it HERE, once, means the UI never has to parse a sensor label and
    // every view (panel, drill-down, verdict) sees the same resolved figure.
    apply_core_temps(s.cpu, s.sensors);

    // SNAPPY: the per-fd socket scan is the single most expensive collector and
    // listening ports change slowly, so refresh it on a wall-clock cadence
    // (~1.5s) and reuse the cached pid_ports_ map in between. First sample
    // always runs it — UNLESS this is a fast startup prime, where we skip it so
    // the first paint doesn't wait on walking every process's fd table.
    if (!fast && due(ports_at_, ms(1500))) sample_ports();
    phase("ports");
    sample_procs(s, sort, top_n, dt);
    phase("procs");
    // Attach the connection table (collected during the throttled ports scan)
    // and stamp each row with its owning process's name for the UI.
    {
        std::unordered_map<int, const std::string*> name_of;
        for (const auto& p : s.procs) name_of[p.pid] = &p.name;
        s.connections = connections_;
        for (auto& c : s.connections)
            if (auto it = name_of.find(c.pid); it != name_of.end()) c.pname = *it->second;
    }

    // PSI pressure (/proc/pressure/*) is a moving average already; ~1s is ample.
    if (due(psi_at_, ms(1000))) { psi_cache_ = Psi{}; sample_psi(psi_cache_); }
    s.psi = psi_cache_;
    phase("psi");

    // Battery: percent/temp crawl, and on Termux the collector forks a whole
    // process (termux-battery-status). Refresh ~every 15s — plenty for a
    // battery gauge, and it keeps process spawns near zero. Skipped on a fast
    // startup prime (leave the stamp at zero so it runs on the first real tick).
    if (!fast && due(battery_at_, ms(15000))) { battery_cache_ = Battery{}; sample_battery(battery_cache_); }
    s.battery = battery_cache_;

    // Wireless (WiFi + cellular) is Termux-only and each helper forks a
    // process; refresh ~every 10s. On desktop Linux sample_wireless is a
    // no-op, so this costs nothing there. Also skipped on a fast prime.
    if (!fast && due(wireless_at_, ms(10000))) { wireless_cache_ = Wireless{}; sample_wireless(wireless_cache_); }
    s.wireless = wireless_cache_;

    // Publish GPU data only after the worker has stopped mutating its cache.
    // Joining this late maximizes overlap with all independent collectors.
    if (gpu_worker) {
        gpu_worker->join();
        if (kPhase)
            std::fprintf(stderr, "  %-12s %8.3f ms (overlapped)\n",
                         "gpu", pms(gpu_started, pnow()));
        if (gpu_error) std::rethrow_exception(gpu_error);
        gpus_cache_ = std::move(gpu_next);
    }
    s.gpus = gpus_cache_;

    s.verdict = judge(s, dt);

    first_ = false;
    return s;
}

}  // namespace rockbottom
