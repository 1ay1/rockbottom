// widgets/mem_panel.hpp — memory card: RAM + swap meters with a
// cache/buffers/available breakdown line.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"
#include "meter.hpp"
#include "panel.hpp"

#include <string>
#include <vector>

namespace bottom::ui {

class MemPanel {
    const MemInfo& mem_;

public:
    explicit MemPanel(const MemInfo& m) : mem_(m) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;
        const double mf = mem_.usage().v;

        rows.push_back((h(
            text("RAM") | Bold | fgc(pal::mem_ac) | w_<4>,
            Meter{mf}.width(24),
            text(fmt::pct(mf)) | nowrap | Bold | fgc(load_color(mf)) | w_<5>,
            text(humanize_bytes(mem_.used) + " / " + humanize_bytes(mem_.total))
                | nowrap | fgc(pal::text)
        ) | gap(1)).build());

        rows.push_back((h(
            text("    ") | w_<4>,
            text("cache ") | nowrap | fgc(pal::dim),
            text(humanize_bytes(mem_.cached)) | nowrap | fgc(pal::teal),
            text("  buf ") | nowrap | fgc(pal::dim),
            text(humanize_bytes(mem_.buffers)) | nowrap | fgc(pal::teal),
            text("  free ") | nowrap | fgc(pal::dim),
            text(humanize_bytes(mem_.available)) | nowrap | fgc(pal::good)
        )).build());

        if (mem_.swap_total.value > 0) {
            const double sf = mem_.swap_usage().v;
            const Color sc = sf > 0.3 ? pal::crit : pal::mauve;
            rows.push_back((h(
                text("SWP") | Bold | fgc(pal::mem_ac) | w_<4>,
                Meter{sf}.width(24).color(sc),
                text(fmt::pct(sf)) | nowrap | fgc(sf > 0.3 ? pal::crit : pal::dim) | w_<5>,
                text(humanize_bytes(mem_.swap_used) + " / " + humanize_bytes(mem_.swap_total))
                    | nowrap | fgc(pal::text)
            ) | gap(1)).build());
        }

        return Panel("▤", "MEMORY", pal::mem_ac)
            .chip(fmt::pct(mf) + " used")(std::move(rows));
    }
};

}  // namespace bottom::ui
