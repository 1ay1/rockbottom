// widgets/hit_ids.hpp — rockbottom's hit-region identity scheme.
//
// Every clickable target tags its box with `| hit(id)` in the widget, and
// the mouse handler resolves a click via maya::hit_test(x, y) — the id comes
// from the SAME layout pass that painted the pixels, so it can never drift
// out of lockstep with a restyle (which is exactly what the old hand-mirrored
// coordinate math kept doing). Kinds are a flat enum; parameterized targets
// (a process row, a sort column, a domain tab, a footer action) pack their
// parameter into the low 32 bits via maya::hit_id(kind, index).

#pragma once

#include <maya/maya.hpp>

#include "../../core/sampler.hpp"   // SortKey
#include "detail_kind.hpp"          // ui::Detail

namespace rockbottom::ui {

enum HitKind : std::uint32_t {
    HK_None = 0,
    HK_ProcRow,      // index = row index in the filtered view
    HK_SortCol,      // index = SortKey
    HK_FooterAct,    // index = FooterAct
    HK_DetailTab,    // index = ui::Detail
    HK_BandPanel,    // index = ui::Detail (top-band domain card)
    HK_DetailBody,   // the drill-down pane body (click closes)
    HK_DetailRow,    // index = body-row index (hover highlight)
};

// Footer actions that a click can trigger. Kept here (not nested in App) so
// both the footer widget and the dispatcher name the same values.
enum class FooterAct : std::uint32_t { Quit, Filter, End, Kill, Sort, Pause, Help };

[[nodiscard]] inline maya::HitId hit_proc_row(int idx) {
    return maya::hit_id(HK_ProcRow, static_cast<std::uint32_t>(idx));
}
[[nodiscard]] inline maya::HitId hit_sort(SortKey k) {
    return maya::hit_id(HK_SortCol, static_cast<std::uint32_t>(k));
}
[[nodiscard]] inline maya::HitId hit_footer(FooterAct a) {
    return maya::hit_id(HK_FooterAct, static_cast<std::uint32_t>(a));
}
[[nodiscard]] inline maya::HitId hit_tab(Detail d) {
    return maya::hit_id(HK_DetailTab, static_cast<std::uint32_t>(d));
}
[[nodiscard]] inline maya::HitId hit_band(Detail d) {
    return maya::hit_id(HK_BandPanel, static_cast<std::uint32_t>(d));
}
[[nodiscard]] inline maya::HitId hit_detail_body() {
    return maya::hit_id(HK_DetailBody, 0);
}
[[nodiscard]] inline maya::HitId hit_detail_row(int idx) {
    return maya::hit_id(HK_DetailRow, static_cast<std::uint32_t>(idx));
}

}  // namespace rockbottom::ui
