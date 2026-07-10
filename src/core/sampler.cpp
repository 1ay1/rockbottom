// sampler.cpp — Orchestration only. Per-domain collection lives in
// collectors/*.cpp; this file wires them together and owns lifecycle.

#include "sampler.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
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
    sample_ports();
    sample_procs(s, sort, top_n, dt);
    sample_psi(s.psi);
    sample_battery(s.battery);
    s.verdict = judge(s);

    first_ = false;
    return s;
}

}  // namespace rockbottom
