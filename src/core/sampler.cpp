// sampler.cpp — Orchestration only. Per-domain collection lives in
// collectors/*.cpp; this file wires them together and owns lifecycle.

#include "sampler.hpp"
#include "procfs.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <sys/utsname.h>
#include <unistd.h>

namespace bottom {

using namespace procfs;

Sampler::Sampler() {
    clk_tck_ = ::sysconf(_SC_CLK_TCK);
    if (clk_tck_ <= 0) clk_tck_ = 100;
    page_size_ = ::sysconf(_SC_PAGESIZE);
    if (page_size_ <= 0) page_size_ = 4096;
    read_static();
}

void Sampler::read_static() {
    char host[256] = {};
    if (::gethostname(host, sizeof host - 1) == 0) hostname_ = host;

    utsname u{};
    if (::uname(&u) == 0) kernel_ = std::string(u.sysname) + " " + u.release;

    std::ifstream ci("/proc/cpuinfo");
    std::string line;
    int cores = 0;
    while (std::getline(ci, line)) {
        if (line.rfind("processor", 0) == 0) ++cores;
        else if (cpu_model_.empty() && line.rfind("model name", 0) == 0) {
            auto c = line.find(':');
            if (c != std::string::npos) cpu_model_ = trim(line.substr(c + 1));
        }
    }
    ncpu_ = std::max(1, cores);
    if (cpu_model_.empty()) cpu_model_ = "CPU";

    std::ifstream mi("/proc/meminfo");
    while (std::getline(mi, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            std::uint64_t kb = 0;
            std::sscanf(line.c_str(), "MemTotal: %lu kB", &kb);
            ram_total_ = Bytes{kb * 1024};
            break;
        }
    }
}

Snapshot Sampler::sample(SortKey sort, int top_n) {
    auto now = std::chrono::steady_clock::now();
    double dt = first_ ? 0.0 : std::chrono::duration<double>(now - last_time_).count();
    if (dt <= 0 && !first_) dt = 0.001;
    last_time_ = now;

    Snapshot s;
    s.hostname = hostname_;
    s.kernel = kernel_;
    s.uptime_sec = static_cast<std::uint64_t>(
        std::strtod(first_line(slurp("/proc/uptime")).c_str(), nullptr));

    sample_cpu(s.cpu);
    sample_mem(s.mem);
    sample_mem_rates(s.mem, dt);
    sample_disks(s.disks);
    sample_disk_io(s.disk_io, dt);
    sample_net(s.nets, dt);
    sample_procs(s, sort, top_n, dt);
    sample_psi(s.psi);
    sample_battery(s.battery);
    s.verdict = judge(s);

    first_ = false;
    return s;
}

}  // namespace bottom
