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
#include "graph.hpp"
#include "bars.hpp"
#include "panel.hpp"

#include <string>
#include <vector>

namespace rockbottom::ui {

class MemPanel {
    const MemInfo& mem_;
    int graph_h_ = 0;   // >0: draw a usage area-graph this tall above the meters
    float grow_ = 0;    // >0: panel fills its flex slot; graph fills the slack

public:
    explicit MemPanel(const MemInfo& m, int graph_h = 0)
        : mem_(m), graph_h_(std::max(0, graph_h)) {}

    // Fill mode: the panel grows to its flex slot and the usage mountain
    // fills whatever height is left after the meters — no graph_h estimate
    // threaded down from the layout, so nothing can drift. (Named `expand`,
    // not `grow`, so it doesn't shadow dsl::grow inside build().)
    MemPanel& expand(float g) { grow_ = g; return *this; }

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

        // Fill mode: a usage mountain that expands to consume the height left
        // after the meters. fill() hands the render the REAL allocated (w, h),
        // so the graph is exactly as tall as its slot — the app never has to
        // estimate a graph_h that then drifts from the real box.
        // Memory is a slowly-varying LEVEL — a block-column histogram reads
        // it as discrete bars (each sample its own column) rather than a
        // smeared braille mountain, and stays visually distinct from the CPU
        // panel's line trace right above it.
        if (grow_ > 0) {
            const float* hist = mem_.usage_history.data();
            const int hlen = mem_.hist_len;
            rows.push_back(fill([hist, hlen](int w, int ah) -> Element {
                using namespace maya;
                using namespace maya::dsl;
                if (ah < 2) return blank().build();
                const int cells = std::max(1, w - 3 - 1);   // y-axis(3) + gap(1)
                BarChart g{hist, hlen};
                g.cells(cells).rows(ah).color(pal::mem_ac);
                return (h(y_axis(ah, 100.0, 3), Element{g.build_fixed()})
                        | gap(1)).build();
            }, 0, 2));
        }
        // Wide/graph mode: a usage-over-time histogram above the meters, so a
        // slow leak is visible as a rising trend, not just a single number.
        // A left y-axis (100 at top, 0 at the floor) makes the height mean
        // something instead of being an unlabelled squiggle.
        else if (graph_h_ >= 2) {
            BarChart g{mem_.usage_history.data(), mem_.hist_len};
            g.fill().rows(graph_h_).color(pal::mem_ac);
            rows.push_back((h(
                y_axis(graph_h_, 100.0, 3),
                Element{g} | grow(1)
            ) | gap(1) | height(graph_h_)).build());
        }

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
            .grow(grow_)
            .chip("cache " + humanize_bytes(mem_.cached)
                  + "  free " + humanize_bytes(mem_.available)
                  + "  ·  " + fmt::pct(mf) + " used")(std::move(rows));
    }
};

}  // namespace rockbottom::ui
