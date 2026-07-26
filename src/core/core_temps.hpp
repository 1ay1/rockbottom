// core/core_temps.hpp — attaching die temperatures to logical cores, as pure
// logic.
//
// The kernel gives us a bag of labelled sensors and a set of cores; deciding
// which reading belongs to which core is fiddly, platform-dependent, and
// exactly the kind of thing that breaks silently — a temperature filed onto
// the wrong core still renders as a perfectly plausible number.
//
// Three resolution steps, strongest evidence first:
//
//   1. PER-CORE sensors. Intel coretemp / AMD zenpower label a sensor "Core 5",
//      naming a PHYSICAL core, not a logical cpu. With SMT physical core 5
//      backs logical cpus 10 and 11, so we fan out through the sibling map.
//      Critically the label mirrors the kernel's RAW topology/core_id, which is
//      not necessarily 0..n-1 (arm64 numbers by MPIDR affinity, hybrid x86
//      leaves gaps) — hence a map rather than an index.
//
//   2. PER-CLUSTER sensors. Apple Silicon publishes no per-core figure at all;
//      the SMC exposes one temperature per cluster (Tp** performance, Te**
//      efficiency). That is still the honest answer for every core in that
//      cluster, and it is what turns a column of dashes into real data. Only
//      cores still lacking a reading are filled, so step 1 always wins.
//
//   3. PACKAGE. The single figure on the panel's border chip: the hottest
//      CPU-zone reading, because the question it answers is "is anything
//      cooking?" and a mean is precisely what hides that.
//
// Pure, so the tests can drive every branch without a machine that has the
// hardware in question.

#pragma once

#include "metrics.hpp"

#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace rockbottom {

// `core_id_siblings` maps the kernel's raw topology/core_id to the logical cpus
// sharing that physical core. Empty means the machine reported no topology, in
// which case a per-core label falls back to a direct index.
inline void resolve_core_temps(CpuInfo& cpu,
                               const std::vector<Sensor>& sensors,
                               const std::unordered_map<int, std::vector<int>>& core_id_siblings) {
    const int n = static_cast<int>(cpu.cores.size());
    if (n == 0) return;
    for (CpuCore& c : cpu.cores) c.temp_c = 0;

    // ── 1. per-core sensors ──
    for (const Sensor& sn : sensors) {
        if (sn.zone != "cpu" || sn.temp_c <= 0) continue;
        // Must name a core AND carry an index: "Core 5", "core5", "Core 5 Temp".
        // A package/die/Tctl sensor has no per-core meaning and is skipped —
        // it already rides the panel's border chip as CpuInfo::temp_c.
        const std::string& lb = sn.label;
        if (lb.find("ore") == std::string::npos) continue;
        const std::size_t p = lb.find_first_of("0123456789");
        if (p == std::string::npos) continue;
        const int core_id = std::atoi(lb.c_str() + static_cast<long>(p));

        auto it = core_id_siblings.find(core_id);
        if (it != core_id_siblings.end()) {
            for (int lg : it->second)
                if (lg >= 0 && lg < n) cpu.cores[static_cast<std::size_t>(lg)].temp_c = sn.temp_c;
        } else if (core_id_siblings.empty() && core_id >= 0 && core_id < n) {
            // No topology available (container with a masked /sys, exotic
            // kernel): the historic 1:1 reading is still better than nothing.
            cpu.cores[static_cast<std::size_t>(core_id)].temp_c = sn.temp_c;
        }
    }

    // ── 2. per-cluster fallback ──
    for (const Sensor& sn : sensors) {
        if (sn.zone != "cpu" || sn.temp_c <= 0) continue;
        // Case-folded: the label's spelling is a per-platform choice ("CPU
        // performance" on macOS), and this must not depend on it.
        std::string lower = sn.label;
        for (char& ch : lower)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

        CoreKind want = CoreKind::Unknown;
        if (lower.find("performance") != std::string::npos) want = CoreKind::Perf;
        else if (lower.find("efficien") != std::string::npos) want = CoreKind::Eff;
        if (want == CoreKind::Unknown) continue;

        for (CpuCore& c : cpu.cores)
            if (c.kind == want && c.temp_c <= 0) c.temp_c = sn.temp_c;
    }

    // ── 3. package ──
    // Platforms exposing a real package sensor set this in their own collector;
    // only fill in when nothing did.
    if (cpu.temp_c <= 0) {
        float hottest = 0;
        for (const Sensor& sn : sensors)
            if (sn.zone == "cpu" && sn.temp_c > hottest) hottest = sn.temp_c;
        cpu.temp_c = hottest;
    }
}

}  // namespace rockbottom
