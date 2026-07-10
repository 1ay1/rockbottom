// collectors/cpu.cpp — /proc/stat + /sys cpufreq + thermal zones.

#include "../../sampler.hpp"
#include "procfs.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <unistd.h>

namespace rockbottom {

using namespace procfs;

// Static machine facts, read once at construction: hostname + kernel (POSIX),
// core count + CPU model (/proc/cpuinfo), and total RAM (/proc/meminfo).
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

// Seconds since boot, from the first field of /proc/uptime.
std::uint64_t Sampler::uptime_sec() const {
    return static_cast<std::uint64_t>(
        std::strtod(first_line(slurp("/proc/uptime")).c_str(), nullptr));
}

void Sampler::sample_cpu(CpuInfo& cpu) {
    cpu.model = cpu_model_;
    cpu.logical = ncpu_;

    std::ifstream st("/proc/stat");
    std::string line;
    std::vector<CpuTimes> cores;
    CpuTimes agg{};
    std::uint64_t agg_iowait = 0;
    while (std::getline(st, line)) {
        if (line.rfind("cpu", 0) != 0) break;
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        std::array<std::uint64_t, 10> v{};
        int n = 0;
        while (n < 10 && (ss >> v[static_cast<std::size_t>(n)])) ++n;
        std::uint64_t idle = v[3] + v[4];  // idle + iowait
        std::uint64_t total = 0;
        for (int i = 0; i < n; ++i) total += v[static_cast<std::size_t>(i)];
        CpuTimes t{idle, total};
        if (tag == "cpu") { agg = t; agg_iowait = v[4]; }
        else cores.push_back(t);
    }

    // Busy fraction = 1 - Δidle/Δtotal across the interval.
    auto busy = [](CpuTimes now, CpuTimes prev) -> Ratio {
        std::uint64_t dt = now.total - prev.total;
        std::uint64_t di = now.idle - prev.idle;
        if (dt == 0) return Ratio{0};
        return Ratio{1.0 - static_cast<double>(di) / static_cast<double>(dt)};
    };

    if (!first_) {
        cpu.total = busy(agg, prev_total_);
        // iowait fraction of the interval — how long cores twiddled thumbs
        // waiting for block devices while runnable work existed.
        std::uint64_t dt = agg.total - prev_total_.total;
        std::uint64_t dw = agg_iowait > prev_iowait_ ? agg_iowait - prev_iowait_ : 0;
        if (dt > 0) cpu.iowait = Ratio{static_cast<double>(dw) / static_cast<double>(dt)};
    }
    prev_total_ = agg;
    prev_iowait_ = agg_iowait;

    cpu.cores.resize(cores.size());
    if (prev_cores_.size() != cores.size()) prev_cores_.assign(cores.size(), CpuTimes{});

    for (std::size_t i = 0; i < cores.size(); ++i) {
        CpuCore& c = core_hist_[static_cast<int>(i)];
        if (!first_) c.usage = busy(cores[i], prev_cores_[i]);
        prev_cores_[i] = cores[i];

        std::string fp = "/sys/devices/system/cpu/cpu" + std::to_string(i) +
                         "/cpufreq/scaling_cur_freq";
        std::string fs = first_line(slurp(fp.c_str()));
        if (!fs.empty()) c.freq = Hertz{std::strtoull(fs.c_str(), nullptr, 10) * 1000};

        push_hist(c.history, c.hist_len, static_cast<float>(c.usage.v));
        cpu.cores[i] = c;
    }

    push_hist(total_hist_, total_hist_len_, static_cast<float>(cpu.total.v));
    cpu.total_history = total_hist_;
    cpu.total_hist_len = total_hist_len_;

    std::ifstream la("/proc/loadavg");
    la >> cpu.loadavg[0] >> cpu.loadavg[1] >> cpu.loadavg[2];

    // Temperature — first CPU/package-ish thermal zone that answers.
    for (int z = 0; z < 16; ++z) {
        std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(z);
        std::string type = trim(first_line(slurp((base + "/type").c_str())));
        if (type.empty()) break;
        if (type.find("pkg") != std::string::npos || type.find("cpu") != std::string::npos ||
            type == "acpitz" || type.find("coretemp") != std::string::npos) {
            std::string t = first_line(slurp((base + "/temp").c_str()));
            if (!t.empty()) { cpu.temp_c = std::strtof(t.c_str(), nullptr) / 1000.0f; break; }
        }
    }
}

}  // namespace rockbottom
