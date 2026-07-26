// sampler.cpp — Orchestration only. Per-domain collection lives in
// collectors/*.cpp; this file wires them together and owns lifecycle.

#include "sampler.hpp"
#include "core_temps.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/resource.h>
#include <unistd.h>

namespace rockbottom {

// signal_process is pure POSIX (kill(2)) — identical on Linux and macOS — so it
// lives in the OS-agnostic orchestrator rather than a per-platform backend.
std::string signal_process(int pid, int sig) {
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

    // Fast, genuinely per-tick metrics: CPU, memory, network, disk I/O rates,
    // GPU. These change every frame and must run every tick.
    sample_cpu(s.cpu);
    sample_mem(s.mem);
    sample_mem_rates(s.mem, dt);
    sample_disk_io(s.disk_io, dt);
    sample_net(s.nets, dt);
    // GPU can fork nvidia-smi on desktop NVIDIA boxes; skip it on the fast
    // startup prime so the first paint never blocks on a subprocess. It runs
    // on the first background tick.
    if (!fast) sample_gpu(s.gpus);

    // Disk CAPACITY (statvfs per mount) barely moves — refresh ~every 5s.
    if (due(disks_at_, ms(5000))) { disks_cache_.clear(); sample_disks(disks_cache_); }
    s.disks = disks_cache_;

    // Hardware temperatures drift slowly — refresh ~every 2s.
    if (due(sensors_at_, ms(2000))) { sensors_cache_.clear(); sample_sensors(sensors_cache_); }
    s.sensors = sensors_cache_;

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
    sample_procs(s, sort, top_n, dt);
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

    s.verdict = judge(s, dt);

    first_ = false;
    return s;
}

}  // namespace rockbottom
