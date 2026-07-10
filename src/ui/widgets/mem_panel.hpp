// widgets/mem_panel.hpp — memory card: RAM + swap meters. Width-responsive:
// the meter always fills the slack; the % column and the trailing byte
// figures drop out first when the panel gets tight, so the bar itself never
// starves.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"
#include "meter.hpp"
#include "panel.hpp"

#include <string>
#include <vector>

namespace rockbottom::ui {

class MemPanel {
    const MemInfo& mem_;

public:
    explicit MemPanel(const MemInfo& m) : mem_(m) {}

    operator maya::Element() const { return build(); }

    // One responsive row: label + (% if room) + fill meter + (bytes if room).
    static maya::Element row(std::string label, double frac, std::string bytes,
                             maya::Color label_c, maya::Color pct_c,
                             std::optional<maya::Color> meter_c) {
        using namespace maya;
        using namespace maya::dsl;
        return Element{ComponentElement{
            .render = [=](int w, int) -> Element {
                const bool show_bytes = w >= 30;
                const bool show_pct   = w >= 16;
                const int  bw = 14;
                int used = 4 + (show_pct ? 6 : 0) + (show_bytes ? bw + 1 : 0);
                int mw = std::max(4, w - used);

                Meter m{frac};
                m.width(mw);
                if (meter_c) m.color(*meter_c);

                std::vector<Element> cols;
                cols.push_back((text(label) | Bold | fgc(label_c) | w_<4>).build());
                if (show_pct)
                    cols.push_back((text(fmt::pct_pad(frac)) | nowrap | Bold
                                    | fgc(pct_c) | w_<5>).build());
                cols.push_back(m.build_fixed());
                if (show_bytes)
                    cols.push_back((text(bytes) | nowrap | fgc(pal::text)).build());
                return (h(std::move(cols)) | gap(1)).build();
            },
        }};
    }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;
        const double mf = mem_.usage().v;

        rows.push_back(row("RAM", mf,
            humanize_bytes(mem_.used) + " / " + humanize_bytes(mem_.total),
            pal::mem_ac, load_color(mf), std::nullopt));

        if (mem_.swap_total.value > 0) {
            const double sf = mem_.swap_usage().v;
            // Occupied swap is not distress — ACTIVE PAGING is. Color the bar
            // by the live swap traffic: quiet mauve when idle, escalating
            // through warn/hot/crit as pages start moving.
            const double traffic = mem_.swap_in.per_sec + mem_.swap_out.per_sec;
            Color sc  = pal::mauve;   // bar
            Color pc  = pal::dim;     // % figure
            if      (traffic > 8.0 * 1024 * 1024) { sc = pal::crit; pc = sc; }  // thrashing
            else if (traffic > 1.0 * 1024 * 1024) { sc = pal::hot;  pc = sc; }  // straining
            else if (traffic > 64.0 * 1024)       { sc = pal::warn; pc = sc; }  // trickle
            std::string tail = traffic > 1024
                ? "paging " + humanize_rate(ByteRate{traffic})
                : humanize_bytes(mem_.swap_used) + " / " + humanize_bytes(mem_.swap_total);
            rows.push_back(row("SWP", sf, tail, pal::mem_ac, pc, sc));
        }

        return Panel("▤", "MEMORY", pal::mem_ac)
            .chip("cache " + humanize_bytes(mem_.cached)
                  + "  free " + humanize_bytes(mem_.available)
                  + "  ·  " + fmt::pct(mf) + " used")(std::move(rows));
    }
};

}  // namespace rockbottom::ui
