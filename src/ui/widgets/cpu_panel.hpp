// widgets/cpu_panel.hpp — CPU card: total meter + per-core meters and
// value-colored history sparklines, temperature chip on the border.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"
#include "meter.hpp"
#include "spark.hpp"
#include "graph.hpp"
#include "panel.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace bottom::ui {

class CpuPanel {
    const CpuInfo& cpu_;
    int cols_;      // 2 = roomy (meter+spark), 3 = compact (meter only)
    int graph_w_;   // width of the ALL history graph (cells)
    int graph_h_;   // height of the ALL graph (rows); 0 = skip it

public:
    explicit CpuPanel(const CpuInfo& c, int cols = 2, int graph_w = 46, int graph_h = 4)
        : cpu_(c), cols_(std::max(2, cols)),
          graph_w_(std::max(8, graph_w)), graph_h_(std::max(0, graph_h)) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;

        // ── Big picture first: braille area+line graph of total CPU — the
        // "what has the machine been doing" trace — with the live meter and
        // bold % stacked to its left. ──
        const double tf = cpu_.total.v;
        if (graph_h_ >= 2) {
            rows.push_back((h(
                v(
                    h(text("ALL") | Bold | fgc(pal::cpu_ac) | w_<4>,
                      text(fmt::pct_pad(tf)) | nowrap | Bold | fgc(load_color(tf)) | w_<5>
                    ) | gap(1),
                    blank(),
                    Meter{tf}.width(10),
                    blank()
                ),
                Graph{cpu_.total_history.data(), cpu_.total_hist_len}.cells(graph_w_).rows(graph_h_)
            ) | gap(2) | height(graph_h_)).build());
            rows.push_back(blank());
        } else {
            // No room for the mountain — keep the live ALL meter as one row.
            rows.push_back((h(
                text("ALL") | Bold | fgc(pal::cpu_ac) | w_<4>,
                text(fmt::pct_pad(tf)) | nowrap | Bold | fgc(load_color(tf)) | w_<5>,
                Element{Meter{tf}.fill()} | grow(1)
            ) | gap(1)).build());
            rows.push_back(blank());
        }

        // ── Per-core grid: cols_ equal columns, one line each. A clean
        // number + right-aligned % + a meter that fills the column. No spark
        // fragments, no dark groove blocks dominating idle cores. ──
        const int n = static_cast<int>(cpu_.cores.size());
        const int per_col = (n + cols_ - 1) / cols_;
        auto cell = [&](int i) -> Element {
            const CpuCore& c = cpu_.cores[static_cast<std::size_t>(i)];
            const double f = c.usage.v;
            char id[8];
            std::snprintf(id, sizeof id, "%2d", i);
            return (h(
                text(id) | nowrap | fgc(pal::cpu_ac) | w_<3>,
                text(fmt::pct_pad(f)) | nowrap | fgc(load_color(f)) | w_<4>,
                Element{Meter{f}.fill().track(pal::bg_panel)} | grow(1)
            ) | gap(1)).build();
        };
        for (int r = 0; r < per_col; ++r) {
            std::vector<Element> line;
            for (int col = 0; col < cols_; ++col) {
                int i = col * per_col + r;
                if (i < n) line.push_back(Element{cell(i)} | grow(1));
                else       line.push_back(Element{blank()} | grow(1));
            }
            rows.push_back((h(line) | gap(3)).build());
        }

        // Temp chip on the border.
        std::string chip;
        if (cpu_.temp_c > 1) chip = std::to_string(static_cast<int>(cpu_.temp_c)) + "°C";

        return Panel("◈", "CPU · " + fmt::short_model(cpu_.model), pal::cpu_ac)
            .chip(chip)(std::move(rows));
    }
};

}  // namespace bottom::ui
