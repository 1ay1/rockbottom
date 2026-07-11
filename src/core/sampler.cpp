// sampler.cpp — Orchestration only. Per-domain collection lives in
// collectors/*.cpp; this file wires them together and owns lifecycle.

#include "sampler.hpp"

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

Snapshot Sampler::sample(SortKey sort, int top_n) {
    auto now = std::chrono::steady_clock::now();
    double dt = first_ ? 0.0 : std::chrono::duration<double>(now - last_time_).count();
    if (dt <= 0 && !first_) dt = 0.001;
    last_time_ = now;

    Snapshot s;
    s.hostname = hostname_;
    s.kernel = kernel_;
    s.uptime_sec = uptime_sec();

    sample_cpu(s.cpu);
    sample_mem(s.mem);
    sample_mem_rates(s.mem, dt);
    sample_disks(s.disks);
    sample_disk_io(s.disk_io, dt);
    sample_net(s.nets, dt);
    sample_gpu(s.gpus);
    sample_sensors(s.sensors);
    // SNAPPY: the per-fd socket scan is the single most expensive collector and
    // listening ports change slowly, so we refresh it only every 4th sample and
    // reuse the cached pid_ports_ map in between. First sample always runs it.
    if (first_ || (ports_tick_++ % 4) == 0) sample_ports();
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
    sample_psi(s.psi);
    sample_battery(s.battery);
    s.verdict = judge(s);

    first_ = false;
    return s;
}

}  // namespace rockbottom
