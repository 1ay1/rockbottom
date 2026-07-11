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

namespace rockbottom::ui {

class CpuPanel {
    const CpuInfo& cpu_;
    const MemInfo* mem_ = nullptr;   // optional: overlay RAM on the ALL graph
    int cols_;      // 2 = roomy (meter+spark), 3 = compact (meter only)
    int graph_w_;   // width of the ALL history graph (cells)
    int graph_h_;   // height of the ALL graph (rows); 0 = skip it
    bool heat_;     // narrow mode: cores as a one-row heat strip, not meters

public:
    explicit CpuPanel(const CpuInfo& c, int cols = 2, int graph_w = 46, int graph_h = 4,
                      const MemInfo* mem = nullptr, bool heat = false)
        : cpu_(c), mem_(mem), cols_(std::max(2, cols)),
          graph_w_(std::max(8, graph_w)), graph_h_(std::max(0, graph_h)), heat_(heat) {}

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
            // Header line: ALL + live % — and, when RAM is overlaid, a small
            // legend so the second (mauve) trace is unambiguous.
            std::vector<Element> hdr;
            hdr.push_back((text("ALL") | Bold | fgc(pal::cpu_ac) | w_<4>).build());
            hdr.push_back((text(fmt::pct_pad(tf)) | nowrap | Bold | fgc(load_color(tf)) | w_<5>).build());
            if (mem_) {
                // Legend swatches must be the EXACT colors the traces are
                // inked with: the cpu trace follows the load gradient (green
                // → amber → red), the ram overlay is always mauve.
                hdr.push_back((Element{blank()} | grow(1)).build());
                hdr.push_back((text("── cpu ") | nowrap | Bold | fgc(load_color(tf))).build());
                hdr.push_back((text(fmt::pct_pad(tf)) | nowrap | fgc(load_color(tf))).build());
                hdr.push_back((text("  ── ram ") | nowrap | Bold | fgc(pal::mem_ac)).build());
                hdr.push_back((text(fmt::pct_pad(mem_->usage().v)) | nowrap | fgc(pal::mem_ac)).build());
            }
            rows.push_back((h(std::move(hdr)) | gap(1)).build());
            // Graph with a labelled left y-axis: 100/75/50/25/0 down the side.
            Graph g{cpu_.total_history.data(), cpu_.total_hist_len};
            g.cells(graph_w_).rows(graph_h_);
            if (mem_) g.overlay(mem_->usage_history.data(), mem_->hist_len, pal::mem_ac);
            rows.push_back((h(
                y_axis(graph_h_, 100.0, 3),
                std::move(g)
            ) | gap(1) | height(graph_h_)).build());
        } else {
            // No room for the mountain — keep the live ALL meter as one row.
            rows.push_back((h(
                text("ALL") | Bold | fgc(pal::cpu_ac) | w_<4>,
                text(fmt::pct_pad(tf)) | nowrap | Bold | fgc(load_color(tf)) | w_<5>,
                Element{Meter{tf}.fill()} | grow(1)
            ) | gap(1)).build());
        }

        // ── Per-core view ──
        const int n = static_cast<int>(cpu_.cores.size());
        if (heat_) {
            // Narrow mode: one heat-strip row (maya Heatmap idiom) — each core
            // is a 2-cell block colored by its load. Denser than meters and
            // reads as a single glance-able texture.
            std::string content;
            std::vector<StyledRun> runs;
            for (int i = 0; i < n; ++i) {
                const double f = cpu_.cores[static_cast<std::size_t>(i)].usage.v;
                // Idle cores show a dim groove block so the strip stays a
                // continuous rail; active ones glow through the gradient.
                Color cc = f < 0.03 ? pal::track : load_color(f);
                std::size_t off = content.size();
                content += "██";
                runs.push_back({off, content.size() - off, Style{}.with_fg(cc)});
                if (i + 1 < n) content += ' ';
            }
            rows.push_back((h(
                text("cores") | nowrap | fgc(pal::cpu_ac) | w_<6>,
                Element{TextElement{.content = std::move(content), .style = {},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(runs)}}
            ) | gap(1)).build());
        } else {
        // Meter grid: cols_ equal columns, one line each. A clean number +
        // right-aligned % + a meter that fills the column, and — like the
        // MEM/NET/DISK panels — a load-graded history SPARKLINE trailing the
        // meter so every core shows its recent trend, not just a static bar.
        const int per_col = (n + cols_ - 1) / cols_;
        auto cell = [&](int i) -> Element {
            const CpuCore& c = cpu_.cores[static_cast<std::size_t>(i)];
            const double f = c.usage.v;
            char id[8];
            std::snprintf(id, sizeof id, "%2d", i);
            std::string id_s = id;
            // Copy the ring so the spark owns its data for the frame.
            std::array<float, 48> hist = c.history;
            const int hl = c.hist_len;
            const Color spark_c = f < 0.03 ? mix(load_color(f), pal::bg_panel, 0.45)
                                           : load_color(f);
            return Element{ComponentElement{
                .render = [=](int w, int) -> Element {
                    // id(3) + % (4) + gaps; split the rest between meter and
                    // spark. Only draw the spark when the column is wide
                    // enough that both stay legible.
                    const int fixed = 3 + 4 + 2;      // labels + gaps
                    const int slack = std::max(0, w - fixed);
                    const bool show_spark = slack >= 14;
                    const int spark_w = show_spark ? slack / 3 : 0;
                    std::vector<Element> cc;
                    cc.push_back((text(id_s) | nowrap | fgc(pal::cpu_ac) | w_<3>).build());
                    cc.push_back((text(fmt::pct_pad(f)) | nowrap | fgc(load_color(f)) | w_<4>).build());
                    cc.push_back(Element{Meter{f}.fill()} | grow(1));
                    if (show_spark)
                        cc.push_back(Spark{hist.data(), hl}.cells(spark_w)
                                         .color(spark_c).baseline(true).build_fixed());
                    return (h(std::move(cc)) | gap(1)).build();
                }}};
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
        }

        // Temp chip on the border.
        std::string chip;
        if (cpu_.temp_c > 1) chip = std::to_string(static_cast<int>(cpu_.temp_c)) + "°C";

        return Panel("◈", "CPU · " + fmt::short_model(cpu_.model), pal::cpu_ac)
            .chip(chip)(std::move(rows));
    }
};

}  // namespace rockbottom::ui
