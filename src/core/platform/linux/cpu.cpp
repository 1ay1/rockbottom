// collectors/cpu.cpp — /proc/stat + /sys cpufreq + thermal zones.

#include "../../sampler.hpp"
#include "armid.hpp"
#include "procfs.hpp"
#include "topology.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
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
    std::string hw_impl, hw_part, arm_fallback;
    while (std::getline(ci, line)) {
        if (line.rfind("processor", 0) == 0) ++cores;
        else if (cpu_model_.empty() && line.rfind("model name", 0) == 0) {
            auto c = line.find(':');
            if (c != std::string::npos) cpu_model_ = trim(line.substr(c + 1));
        }
        // arm64 /proc/cpuinfo has NO "model name" line at all — it publishes
        // "CPU implementer"/"CPU part" hex ids instead. Without this every
        // aarch64 box (Ampere, Graviton, Pi, most Android) showed the literal
        // string "CPU" as its processor. Collect the pieces; resolve below.
        else if (line.rfind("CPU implementer", 0) == 0) {
            auto c = line.find(':');
            if (c != std::string::npos && hw_impl.empty()) hw_impl = trim(line.substr(c + 1));
        } else if (line.rfind("CPU part", 0) == 0) {
            auto c = line.find(':');
            if (c != std::string::npos && hw_part.empty()) hw_part = trim(line.substr(c + 1));
        } else if (arm_fallback.empty() &&
                   (line.rfind("Hardware", 0) == 0 || line.rfind("Model", 0) == 0)) {
            // Pi / many SoCs put a human name here; better than a hex pair.
            auto c = line.find(':');
            if (c != std::string::npos) arm_fallback = trim(line.substr(c + 1));
        }
    }
    ncpu_ = std::max(1, cores);

    if (cpu_model_.empty()) cpu_model_ = arm::model_name(hw_impl, hw_part);
    if (cpu_model_.empty()) cpu_model_ = arm_fallback;
    if (cpu_model_.empty()) {
        // Device-tree machines (and arm64 VMs) name the board here.
        std::string dt = trim(first_line(slurp("/proc/device-tree/model")));
        if (!dt.empty()) cpu_model_ = dt;
    }
    if (cpu_model_.empty()) cpu_model_ = "CPU";

    std::ifstream mi("/proc/meminfo");
    while (std::getline(mi, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            // strtoull, not sscanf("%lu"): unsigned long is 32-bit on ILP32
            // targets (armv7 Termux), where %lu into a uint64_t is UB.
            std::uint64_t kb = std::strtoull(line.c_str() + 9, nullptr, 10);
            ram_total_ = Bytes{kb * 1024};
            break;
        }
    }

    probe_topology();
}

namespace {

// Per-cpu current clock from /proc/cpuinfo's "cpu MHz" lines, in Hz.
//
// This is the fallback for machines with no cpufreq driver bound — cloud VMs,
// most virtualised guests, containers with a masked /sys. x86 kernels emit one
// "cpu MHz" line per processor block even there, so the Nth line belongs to
// cpuN. Returns an empty vector when the field is absent (arm64 never has it),
// which the caller reads as "no clock available" and renders as a dash rather
// than inventing a number.
std::vector<std::uint64_t> cpuinfo_mhz(std::size_t n) {
    std::vector<std::uint64_t> out;
    out.reserve(n);
    std::ifstream ci("/proc/cpuinfo");
    std::string line;
    while (std::getline(ci, line) && out.size() < n) {
        if (line.rfind("cpu MHz", 0) != 0) continue;
        const auto c = line.find(':');
        if (c == std::string::npos) continue;
        const double mhz = std::strtod(line.c_str() + c + 1, nullptr);
        out.push_back(mhz > 0 ? static_cast<std::uint64_t>(mhz * 1e6) : 0);
    }
    return out;
}

// Root of the sysfs tree the topology probe reads. Always "" (i.e. the real
// /sys) in normal operation; RB_SYSFS_ROOT redirects it at a captured tree so
// the probe can be exercised against real topologies from machines we don't
// have. Read once; costs one getenv at startup.
const std::string& sysfs_root() {
    static const std::string root = [] {
        const char* e = std::getenv("RB_SYSFS_ROOT");
        return e ? std::string(e) : std::string();
    }();
    return root;
}

}  // namespace

// Probe the static CPU topology (P/E class + physical core id per logical cpu).
//
// The DECISION procedure lives in topology.hpp as a pure function over an
// injected file reader, so it can be tested against captured sysfs layouts;
// this is just the adapter that points it at the real /sys and copies the
// result into the sampler's caches. Runs ONCE at startup: a handful of small
// sysfs reads, nothing forks, and the per-tick path is untouched.
void Sampler::probe_topology() {
    const topo::Topology t = topo::classify(ncpu_, [](const std::string& p) {
        return slurp((sysfs_root() + p).c_str());
    });
    core_kind_     = t.kind;
    core_phys_     = t.phys;
    phys_siblings_ = t.siblings;
    core_id_siblings_ = t.by_core_id;
    perf_label_    = t.perf_label;
    eff_label_     = t.eff_label;
}

// Seconds since boot, from the first field of /proc/uptime. On Android the
// SELinux sandbox blocks /proc/uptime, so fall back to sysinfo(2), which the
// kernel answers directly without a procfs node (uptime field is in seconds).
std::uint64_t Sampler::uptime_sec() const {
    std::string up = first_line(slurp("/proc/uptime"));
    if (!up.empty()) {
        double v = std::strtod(up.c_str(), nullptr);
        if (v > 0) return static_cast<std::uint64_t>(v);
    }
    struct ::sysinfo si{};
    if (::sysinfo(&si) == 0 && si.uptime > 0)
        return static_cast<std::uint64_t>(si.uptime);
    return 0;
}

void Sampler::sample_cpu(CpuInfo& cpu) {
    cpu.model = cpu_model_;
    cpu.logical = ncpu_;

    std::ifstream st("/proc/stat");
    // Android/Termux: /proc/stat is hidden by the SELinux sandbox. Flag it so
    // sample_procs can synthesize aggregate CPU from per-process deltas.
    cpu_stat_ok_ = st.good();
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

    // Busy fraction = 1 - Δidle/Δtotal across the interval. Both deltas are
    // clamped at zero: the kernel documents that per-cpu iowait (folded into
    // our idle) can DECREASE, and an unguarded u64 subtraction would wrap to
    // ~2^64 and paint a bogus 0%/100% frame.
    auto busy = [](CpuTimes now, CpuTimes prev) -> Ratio {
        std::uint64_t dt = now.total > prev.total ? now.total - prev.total : 0;
        std::uint64_t di = now.idle  > prev.idle  ? now.idle  - prev.idle  : 0;
        if (dt == 0) return Ratio{0};
        return Ratio{1.0 - static_cast<double>(di) / static_cast<double>(dt)};
    };

    if (!first_) {
        cpu.total = busy(agg, prev_total_);
        // iowait fraction of the interval — how long cores twiddled thumbs
        // waiting for block devices while runnable work existed.
        std::uint64_t dt = agg.total > prev_total_.total ? agg.total - prev_total_.total : 0;
        std::uint64_t dw = agg_iowait > prev_iowait_ ? agg_iowait - prev_iowait_ : 0;
        if (dt > 0) cpu.iowait = Ratio{static_cast<double>(dw) / static_cast<double>(dt)};
    }
    prev_total_ = agg;
    prev_iowait_ = agg_iowait;

    cpu.cores.resize(cores.size());
    if (prev_cores_.size() != cores.size()) prev_cores_.assign(cores.size(), CpuTimes{});

    // Per-core clock. cpufreq is a DRIVER, not a guarantee: cloud VMs and
    // most virtualised guests expose no /sys/.../cpufreq tree at all (the CI
    // runners show exactly this), and some kernels ship cpuinfo_cur_freq
    // without scaling_cur_freq. Try the accurate source, then the alternate
    // sysfs name, then fall back to the per-cpu "cpu MHz" line in
    // /proc/cpuinfo, which x86 fills in even with no cpufreq driver bound.
    const std::vector<std::uint64_t> cpuinfo_hz =
        cpufreq_missing_ ? cpuinfo_mhz(cores.size()) : std::vector<std::uint64_t>{};
    bool any_cpufreq = false;

    for (std::size_t i = 0; i < cores.size(); ++i) {
        CpuCore& c = core_hist_[static_cast<int>(i)];
        if (!first_) c.usage = busy(cores[i], prev_cores_[i]);
        prev_cores_[i] = cores[i];

        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/";
        std::string fs;
        if (!cpufreq_missing_) {
            fs = first_line(slurp((base + "scaling_cur_freq").c_str()));
            if (fs.empty()) fs = first_line(slurp((base + "cpuinfo_cur_freq").c_str()));
        }
        if (!fs.empty()) {
            c.freq = Hertz{std::strtoull(fs.c_str(), nullptr, 10) * 1000};
            any_cpufreq = true;
        } else if (i < cpuinfo_hz.size() && cpuinfo_hz[i] > 0) {
            c.freq = Hertz{cpuinfo_hz[i]};
        }

        push_hist(c.history, c.hist_len, static_cast<float>(c.usage.v));
        cpu.cores[i] = c;
    }
    // Latch the "no cpufreq here" answer after the first full sweep, so we
    // stop paying two failed opens per core per tick on machines that will
    // never have the tree. Whether a cpufreq driver is bound is static.
    if (!cpufreq_missing_ && !any_cpufreq) cpufreq_missing_ = true;

    // Label each core with its cluster from the topology probed at startup.
    apply_topology(cpu);

    // When /proc/stat is readable, push the real aggregate here. On Android
    // (blocked) sample_procs computes cpu.total from per-process deltas and
    // pushes the ring itself, so we skip it to avoid seeding the graph with 0.
    if (cpu_stat_ok_) {
        push_hist(total_hist_, total_hist_len_, static_cast<float>(cpu.total.v));
    }
    cpu.total_history = total_hist_;
    cpu.total_hist_len = total_hist_len_;

    std::ifstream la("/proc/loadavg");
    loadavg_ok_ = static_cast<bool>(la >> cpu.loadavg[0] >> cpu.loadavg[1] >> cpu.loadavg[2]);
    // /proc/loadavg is blocked on Android; sample_procs backfills a synthetic
    // load from the running-process count + total CPU once it has walked /proc.

    // Temperature — first CPU/package-ish thermal zone that answers. A gap in
    // the zone numbering (an unreadable or hot-unplugged zoneN) must not hide
    // the zones after it, so skip gaps instead of stopping at the first one.
    for (int z = 0; z < 16; ++z) {
        std::string base = "/sys/class/thermal/thermal_zone" + std::to_string(z);
        std::string type = trim(first_line(slurp((base + "/type").c_str())));
        if (type.empty()) continue;
        if (type.find("pkg") != std::string::npos || type.find("cpu") != std::string::npos ||
            type == "acpitz" || type.find("coretemp") != std::string::npos) {
            std::string t = first_line(slurp((base + "/temp").c_str()));
            if (!t.empty()) { cpu.temp_c = std::strtof(t.c_str(), nullptr) / 1000.0f; break; }
        }
    }
}

}  // namespace rockbottom
