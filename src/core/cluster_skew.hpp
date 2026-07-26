// core/cluster_skew.hpp — "is the work on the wrong cluster?", as pure logic.
//
// Issue #3 asks for two things: show which cores are P and which are E, AND
// speak up when heavy work is sitting on the slow ones. The second half is a
// judgement call with thresholds, and thresholds are exactly the kind of thing
// that silently stops firing when someone edits the code around them — the
// condition still compiles, still looks reasonable, and simply never triggers.
//
// So the arithmetic lives here as a free function over plain numbers, with no
// Snapshot and no Sampler, and the tests drive it directly at the boundaries.
// verdict.cpp keeps the prose.

#pragma once

#include "metrics.hpp"

#include <vector>

namespace rockbottom {

struct ClusterLoad {
    double perf_avg = 0;   // mean busy % across the performance cluster
    double eff_avg  = 0;   // mean busy % across the efficiency cluster
    int    perf_n   = 0;
    int    eff_n    = 0;

    // The state issue #3 describes: the efficiency cluster is pinned while
    // performance capacity sits idle next to it.
    //
    // Both bounds matter. Without the eff floor we'd nag about ordinary
    // background work, which is what the E cluster is FOR — a phone at 40% on
    // its little cores is behaving correctly. Without the perf ceiling we'd
    // fire while the whole machine is flat out, where the advice is useless
    // because there is nowhere faster to move anything to. It only helps when
    // there is genuinely idle P capacity to move work onto.
    bool misplaced() const {
        return perf_n > 0 && eff_n > 0 && eff_avg > 70 && perf_avg < 30;
    }
};

inline ClusterLoad cluster_load(const std::vector<CpuCore>& cores) {
    ClusterLoad c;
    double psum = 0, esum = 0;
    for (const CpuCore& k : cores) {
        if (k.kind == CoreKind::Eff)       { esum += k.usage.percent(); ++c.eff_n; }
        else if (k.kind == CoreKind::Perf) { psum += k.usage.percent(); ++c.perf_n; }
    }
    c.perf_avg = c.perf_n ? psum / c.perf_n : 0;
    c.eff_avg  = c.eff_n  ? esum / c.eff_n  : 0;
    return c;
}

}  // namespace rockbottom
