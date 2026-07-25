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
#include <memory>
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
    float grow_ = 0;  // >0: panel fills its flex slot; graph fills the slack

public:
    explicit CpuPanel(const CpuInfo& c, int cols = 2, int graph_w = 46, int graph_h = 4,
                      const MemInfo* mem = nullptr, bool heat = false)
        : cpu_(c), mem_(mem), cols_(std::max(2, cols)),
          graph_w_(std::max(8, graph_w)), graph_h_(std::max(0, graph_h)), heat_(heat) {}

    // Fill mode: the panel grows to its flex slot and the ALL mountain fills
    // whatever height is left after the header + core rows — no graph_h
    // estimate threaded down from the layout, so nothing can drift. (Named
    // `expand`, not `grow`, so it doesn't shadow dsl::grow inside build().)
    CpuPanel& expand(float g) { grow_ = g; return *this; }

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;

        // ── Big picture first: braille area+line graph of total CPU — the
        // "what has the machine been doing" trace — with the live meter and
        // bold % stacked to its left. ──
        const double tf = cpu_.total.v;
        if (grow_ > 0 || graph_h_ >= 2) {
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
        }
        // Fill mode: the mountain expands to consume the height left after
        // the header + core rows. fill() hands the render the REAL (w, h) so
        // the graph is exactly as tall as its slot — no estimate to drift.
        if (grow_ > 0) {
            const float* hist = cpu_.total_history.data();
            const int hlen = cpu_.total_hist_len;
            const float* memh = mem_ ? mem_->usage_history.data() : nullptr;
            const int memlen = mem_ ? mem_->hist_len : 0;
            rows.push_back(fill([hist, hlen, memh, memlen](int w, int ah) -> Element {
                using namespace maya;
                using namespace maya::dsl;
                // No room for even a 2-row mountain — collapse to nothing
                // (height 0) so a tiny band spends its rows on the header +
                // cores strip instead of a dead blank line. The live % still
                // reads on the header row above.
                if (ah < 2) return (blank() | height(0)).build();
                Graph g{hist, hlen};
                // light_fill() lays a faint dot-rain under the trace so the
                // graph reads as an area chart with body — far better than a
                // bare line that scatters into disconnected dots at low load.
                // The sparse rain (one column every few cells) keeps the mauve
                // RAM overlay legible instead of drowning it in a solid wall.
                g.cells(std::max(1, w - 3 - 1)).rows(ah).light_fill();   // y-axis(3) + gap(1)
                if (memh) g.overlay(memh, memlen, pal::mem_ac);
                return (h(y_axis(ah, 100.0, 3), Element{g.build_fixed()})
                        | gap(1)).build();
            }, 0, 2));
        } else if (graph_h_ >= 2) {
            // Graph with a labelled left y-axis: 100/75/50/25/0 down the side.
            Graph g{cpu_.total_history.data(), cpu_.total_hist_len};
            g.cells(graph_w_).rows(graph_h_).light_fill();
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
            //
            // On heterogeneous silicon the two clusters are separated by a
            // divider and the label names the split, so even the narrowest
            // layout still answers "is my work on the fast cores?".
            const bool hetero = cpu_.hetero();
            std::string content;
            std::vector<StyledRun> runs;
            auto emit = [&](int i, bool last) {
                const CpuCore& core = cpu_.cores[static_cast<std::size_t>(i)];
                const double f = core.usage.v;
                // Idle cores show a dim groove block so the strip stays a
                // continuous rail; active ones glow through the gradient.
                Color cc = f < 0.03 ? pal::track : load_color(f);
                // E-core blocks sit one notch back so the cluster reads as
                // texture, without hiding a genuinely hot E core.
                if (hetero && core.kind == CoreKind::Eff)
                    cc = mix(cc, pal::bg_panel, 0.3);
                std::size_t off = content.size();
                content += "██";
                runs.push_back({off, content.size() - off, Style{}.with_fg(cc)});
                if (!last) content += ' ';
            };
            if (hetero) {
                std::vector<int> perf, eff;
                for (int i = 0; i < n; ++i) {
                    if (cpu_.cores[static_cast<std::size_t>(i)].kind == CoreKind::Eff) eff.push_back(i);
                    else perf.push_back(i);
                }
                for (std::size_t k = 0; k < perf.size(); ++k) emit(perf[k], k + 1 == perf.size());
                if (!perf.empty() && !eff.empty()) {
                    std::size_t off = content.size();
                    content += " │ ";
                    runs.push_back({off, content.size() - off, Style{}.with_fg(pal::faint)});
                }
                for (std::size_t k = 0; k < eff.size(); ++k) emit(eff[k], k + 1 == eff.size());
            } else {
                for (int i = 0; i < n; ++i) emit(i, i + 1 == n);
            }
            rows.push_back((h(
                text(hetero ? " P·E " : "cores") | nowrap | fgc(pal::cpu_ac) | w_<6>,
                Element{TextElement{.content = std::move(content), .style = {},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(runs)}}
            ) | gap(1)).build());
        } else {
        // Meter grid: cols_ equal columns, one line each. A clean number +
        // right-aligned % + a meter that fills the column, and — like the
        // MEM/NET/DISK panels — a load-graded history SPARKLINE trailing the
        // meter so every core shows its recent trend, not just a static bar.
        //
        // On heterogeneous silicon each id carries its cluster letter (P/E)
        // and E-core ids are dimmed, so "my build is on the slow cores" is
        // visible from the dashboard without opening the drill-down.
        const bool hetero = cpu_.hetero();
        const int per_col = (n + cols_ - 1) / cols_;
        // Spark width is solved ONCE for the whole grid, not per column: the
        // columns can differ by a cell of layout residue, and a per-column
        // slack/3 turns that into a whole cell of spark drift, which drags
        // every figure right of it out of line (issue #2).
        const int id_w = hetero ? 5 : 3;
        // Snapshot exactly what a cell needs. The grid renders LATER (maya
        // solves the component when it has a width, and may re-render it from
        // its cross-frame cache), by which time `this` and cpu_ may be gone —
        // so the closure must own its data outright, never reference it.
        struct CoreCell {
            std::string id;
            double f = 0;
            Color id_c{}, spark_c{};
            std::array<float, 48> hist{};
            int hl = 0;
        };
        auto cells = std::make_shared<std::vector<CoreCell>>();
        cells->reserve(static_cast<std::size_t>(std::max(0, n)));
        for (int i = 0; i < n; ++i) {
            const CpuCore& c = cpu_.cores[static_cast<std::size_t>(i)];
            const double f = c.usage.v;
            char id[12];
            if (hetero) std::snprintf(id, sizeof id, "%2d·%c", i, c.kind == CoreKind::Eff ? 'E' : 'P');
            else        std::snprintf(id, sizeof id, "%2d", i);
            cells->push_back(CoreCell{
                id, f,
                !hetero || c.kind == CoreKind::Perf ? pal::cpu_ac
                                                    : mix(pal::cpu_ac, pal::dim, 0.55),
                f < 0.03 ? mix(load_color(f), pal::bg_panel, 0.45) : load_color(f),
                c.history, c.hist_len});
        }
        // The whole grid is ONE component so every row is solved against the
        // same column width and the same spark width — the numbers form a
        // straight column down the panel instead of ragging per row.
        const int ncols = cols_;
        rows.push_back(Element{ComponentElement{
            .render = [cells, per_col, ncols, id_w](int w, int) -> Element {
                const int gutter = 3;
                const int col_w = std::max(1, (w - gutter * (ncols - 1)) / ncols);
                // id + % + two gaps are fixed; the rest splits between the
                // elastic meter and the spark.
                const int slack = std::max(0, col_w - (id_w + 4 + 2));
                const int spark_w = slack >= 14 ? slack / 3 : 0;
                const int total = static_cast<int>(cells->size());
                std::vector<Element> grid;
                for (int r = 0; r < per_col; ++r) {
                    std::vector<Element> line;
                    for (int col = 0; col < ncols; ++col) {
                        const int i = col * per_col + r;
                        Element e = Element{blank()};
                        if (i < total) {
                            const CoreCell& cc = (*cells)[static_cast<std::size_t>(i)];
                            std::vector<Element> parts;
                            parts.push_back((text(cc.id) | nowrap | Bold | fgc(cc.id_c) | width(id_w)).build());
                            parts.push_back((text(fmt::pct_pad(cc.f)) | nowrap
                                             | fgc(load_color(cc.f)) | w_<4>).build());
                            parts.push_back(Element{Meter{cc.f}.fill()} | grow(1));
                            if (spark_w > 0)
                                parts.push_back(Spark{cc.hist.data(), cc.hl}.cells(spark_w)
                                                    .color(cc.spark_c).baseline(true).build_fixed());
                            e = (h(std::move(parts)) | gap(1)).build();
                        }
                        line.push_back((Element{std::move(e)} | width(col_w)).build());
                    }
                    grid.push_back((h(std::move(line)) | gap(gutter)).build());
                }
                return v(std::move(grid)).build();
            },
            .measure = [per_col](int max_width) -> Size {
                return {Columns{max_width > 0 ? max_width : 1}, Rows{std::max(1, per_col)}};
            },
        }});
        }

        // Temp chip on the border.
        std::string chip;
        if (cpu_.temp_c > 1) chip = std::to_string(static_cast<int>(cpu_.temp_c)) + "°C";

        return Panel("◈", "CPU · " + fmt::short_model(cpu_.model), pal::cpu_ac)
            .grow(grow_)
            .chip(chip)(std::move(rows));
    }
};

}  // namespace rockbottom::ui
