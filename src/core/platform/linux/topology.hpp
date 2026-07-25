// platform/linux/topology.hpp — CPU topology classification, as pure logic.
//
// Deciding which logical cpus are performance vs efficiency cores is the piece
// that makes heterogeneous machines legible (issue #3), and it is ALL parsing:
// read a few sysfs files, decide. The I/O is one injected `read_file` callback
// so the decision procedure can be driven against captured layouts from
// machines the developer doesn't own — an Alder Lake hybrid, a big.LITTLE
// phone, a masked container /sys — instead of only whatever box compiled it.
//
// Sources, strongest evidence first. The kernel gives a definitive answer on
// modern hardware; we only fall back to inference on older DT-only systems:
//
//   1. /sys/devices/cpu_core/cpus + cpu_atom/cpus — the perf-PMU split the
//      kernel publishes on Intel hybrid parts (Alder Lake onward). Definitive:
//      it is the kernel naming both clusters outright.
//   2. cpuN/cpu_capacity — the scheduler's own capacity figure, populated from
//      the device tree on ARM big.LITTLE / DynamIQ. Max capacity = P cluster.
//   3. cpufreq/cpuinfo_max_freq — inference of last resort: if the clock
//      ceilings fall into distinct tiers, the top tier is the P cluster.
//
// Every source absent (container with a masked /sys, genuinely homogeneous
// machine) yields an empty result, which the caller reads as "homogeneous".

#pragma once

#include "../../metrics.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rockbottom::topo {

// Reads a file and returns its contents, or "" if it doesn't exist.
using ReadFile = std::function<std::string(const std::string& path)>;

struct Topology {
    std::vector<CoreKind> kind;    // by logical cpu; empty = homogeneous
    std::vector<int>      phys;    // logical -> physical core id; empty = unknown
    std::unordered_map<int, std::vector<int>> siblings;   // physical -> logicals
    std::string           perf_label, eff_label;
};

namespace detail {

inline std::string trim_ws(std::string s) {
    const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    std::size_t b = 0, e = s.size();
    while (b < e && ws(s[b])) ++b;
    while (e > b && ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Parse a Linux cpulist ("0-7,16,20-23") into logical cpu indices. This is the
// format every /sys/devices/**/cpus file speaks.
inline std::vector<int> parse_cpulist(const std::string& s) {
    std::vector<int> out;
    const char* p = s.c_str();
    while (*p) {
        char* end = nullptr;
        long a = std::strtol(p, &end, 10);
        if (end == p) break;
        long b = a;
        p = end;
        if (*p == '-') {
            ++p;
            b = std::strtol(p, &end, 10);
            if (end == p) break;
            p = end;
        }
        for (long i = a; i <= b && i < 4096; ++i) out.push_back(static_cast<int>(i));
        while (*p == ',' || *p == ' ' || *p == '\n') ++p;
    }
    return out;
}

}  // namespace detail

// Classify `n` logical cpus. `read` resolves a sysfs path to its contents.
inline Topology classify(int n_cpus, const ReadFile& read) {
    using detail::parse_cpulist;
    using detail::trim_ws;

    Topology t;
    const std::size_t n = static_cast<std::size_t>(std::max(1, n_cpus));
    t.kind.assign(n, CoreKind::Unknown);
    t.phys.assign(n, -1);

    const std::string cpu_base = "/sys/devices/system/cpu/cpu";

    // ── physical core ids (SMT siblings share one) ──
    // topology/core_id is per-PACKAGE, so on a multi-socket box two sockets
    // both have a "core 0". Key by (package, core) and fall back to the raw id
    // when the package file is missing.
    bool any_phys = false;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string base = cpu_base + std::to_string(i) + "/topology/";
        const std::string cid = trim_ws(read(base + "core_id"));
        if (cid.empty()) continue;
        const std::string pkg = trim_ws(read(base + "physical_package_id"));
        const int core = std::atoi(cid.c_str());
        const int sock = pkg.empty() ? 0 : std::atoi(pkg.c_str());
        const int id = sock * 1024 + core;
        t.phys[i] = id;
        t.siblings[id].push_back(static_cast<int>(i));
        any_phys = true;
    }
    if (!any_phys) { t.phys.clear(); t.siblings.clear(); }

    // ── 1. Intel hybrid: the kernel names the two clusters outright ──
    {
        const std::vector<int> big    = parse_cpulist(trim_ws(read("/sys/devices/cpu_core/cpus")));
        const std::vector<int> little = parse_cpulist(trim_ws(read("/sys/devices/cpu_atom/cpus")));
        if (!big.empty() && !little.empty()) {
            for (int i : big)
                if (i >= 0 && i < static_cast<int>(n)) t.kind[static_cast<std::size_t>(i)] = CoreKind::Perf;
            for (int i : little)
                if (i >= 0 && i < static_cast<int>(n)) t.kind[static_cast<std::size_t>(i)] = CoreKind::Eff;
            t.perf_label = "Performance";
            t.eff_label  = "Efficient";
            return t;
        }
    }

    // ── 2 & 3. Two-tier inference from a per-cpu scalar ──
    // Both remaining sources have the same shape: read one number per cpu, and
    // if the values split into tiers, the top tier is the performance cluster.
    // Shared so the two probes can't drift apart.
    auto classify_by = [&](const std::string& suffix) -> bool {
        std::vector<long> v(n, 0);
        long best = 0;
        std::size_t seen = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const std::string s = trim_ws(read(cpu_base + std::to_string(i) + suffix));
            if (s.empty()) continue;
            v[i] = std::strtol(s.c_str(), nullptr, 10);
            if (v[i] > 0) { ++seen; best = std::max(best, v[i]); }
        }
        // Require EVERY cpu to answer: a partial read means we'd silently call
        // the unreadable half "efficiency".
        if (seen < n || best <= 0) return false;
        // Require a real spread before calling a machine heterogeneous. A few
        // percent between cores is binning scatter or a boost-clock quirk, not
        // a cluster; ~15% below the top is comfortably inside every real
        // big.LITTLE / hybrid split and outside every homogeneous part.
        const long cut = best - best / 6;
        bool has_small = false;
        for (std::size_t i = 0; i < n; ++i) if (v[i] < cut) { has_small = true; break; }
        if (!has_small) return false;
        for (std::size_t i = 0; i < n; ++i)
            t.kind[i] = v[i] >= cut ? CoreKind::Perf : CoreKind::Eff;
        return true;
    };

    if (classify_by("/cpu_capacity")) {
        t.perf_label = "Performance"; t.eff_label = "Efficient";
        return t;
    }
    if (classify_by("/cpufreq/cpuinfo_max_freq")) {
        t.perf_label = "Fast"; t.eff_label = "Slow";
        return t;
    }

    t.kind.clear();   // homogeneous, or nothing readable
    return t;
}

}  // namespace rockbottom::topo
