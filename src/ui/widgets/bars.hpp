// widgets/bars.hpp — rockbottom::ui::BarChart, a multi-row vertical block-bar
// histogram.
//
// Where Graph draws a continuous braille TRACE (a curve) and Spark a single
// row of block bars, BarChart stacks the block glyphs (▁▂▃▄▅▆▇█) across
// several rows so each history sample becomes a crisp vertical BAR whose full
// height is the graph height. It reads as a bar chart, not a smeared mountain
// — the ideal idiom for spiky, bursty series (disk I/O) and for a level that
// you want to see as discrete columns (memory).
//
//   BarChart{hist.data(), len}.fill().rows(6).color(pal::mem_ac)
//
// Each sample owns ONE terminal column (so `cells` samples fit `cells` cols);
// when history is shorter than the slot it right-packs (newest on the right)
// and left-pads with blanks, same as Spark. An optional per-value load
// gradient colors each bar by its own height when no flat color is set.

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace rockbottom::ui {

class BarChart {
    std::vector<float> data_;
    int len_ = 0;
    int cells_ = 12;
    int rows_ = 4;
    std::optional<maya::Color> color_;   // nullopt → per-value load gradient
    float gamma_ = 1.0f;                 // perceptual curve for bursty rates
    std::vector<maya::Color> col_colors_; // optional per-sample color (index-aligned to data_)

public:
    BarChart(const float* data, int len) : len_(std::max(0, len)) {
        data_.assign(data, data + len_);
    }

    BarChart& cells(int n)         { cells_ = n; return *this; }   // <=0 → fill
    BarChart& rows(int n)          { rows_ = std::max(1, n); return *this; }
    BarChart& color(maya::Color c) { color_ = c; return *this; }
    // Per-sample colors, index-aligned to the data passed in. Overrides color()
    // per column when present (falls back to color()/gradient for any column
    // beyond the array). Lets one histogram tint each bar by which series
    // dominated that sample (e.g. read vs write).
    BarChart& colors(const maya::Color* c, int n) {
        col_colors_.assign(c, c + std::max(0, n)); return *this;
    }
    BarChart& fill()               { cells_ = 0; return *this; }
    BarChart& gamma(float g)       { gamma_ = std::max(0.05f, g); return *this; }

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        if (cells_ <= 0) {
            BarChart self = *this;
            const int nat_rows = rows_;
            ComponentElement ce{
                .render = [self](int w, int) -> Element {
                    BarChart c = self;
                    c.cells_ = std::max(1, w);
                    return c.build_fixed();
                },
                .measure = [nat_rows](int) -> Size {
                    return {maya::Columns{1}, Rows{nat_rows}};
                },
            };
            ce.layout.grow = 1.0f;
            return Element{std::move(ce)};
        }
        return build_fixed();
    }

    [[nodiscard]] maya::Element build_fixed() const {
        using namespace maya;
        // Eighth-block ladder: index 0..8 maps to how many eighths of a cell
        // are inked from the bottom. 0 = empty (space).
        static constexpr const char* kBar[9] =
            {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

        // Map each of the cells_ display columns to a RANGE of source samples
        // [s0, s1) and reduce it peak-preserving (take the tallest sample in
        // the range). This keeps the graph honest under downsampling: a brief
        // burst that falls between two columns is never dropped the way a
        // single nearest-sample pick would drop it. Two regimes:
        //   • more samples than cells (len_ > cells_): each column spans
        //     several samples — max over them (a scrolling, peak-preserving
        //     window of the most recent cells_ worth of history).
        //   • more cells than samples (cells_ >= len_): each column maps to
        //     one sample (ranges are width ≤ 1), stretching the history to
        //     fill the panel instead of packing on the right.
        // range_of(c) returns [s0, s1); s0>=s1 (empty) marks a blank column
        // when there is no data at all.
        auto range_of = [&](int c) -> std::pair<int,int> {
            if (len_ <= 0 || cells_ <= 0) return {0, 0};
            // Column c covers the normalized span [c/cells_, (c+1)/cells_) of
            // the timeline, mapped onto sample indices [0, len_).
            double a = static_cast<double>(c)     / cells_ * len_;
            double b = static_cast<double>(c + 1) / cells_ * len_;
            int s0 = static_cast<int>(std::floor(a));
            int s1 = static_cast<int>(std::ceil(b));
            s0 = std::clamp(s0, 0, len_ - 1);
            s1 = std::clamp(s1, s0 + 1, len_);   // always cover >=1 sample
            return {s0, s1};
        };

        // Precompute, per column, the value fraction (curved) and the source
        // sample that supplied the peak (for the per-column color).
        std::vector<double> vals(static_cast<std::size_t>(cells_), 0.0);
        std::vector<int>    peak_sample(static_cast<std::size_t>(cells_), -1);
        for (int c = 0; c < cells_; ++c) {
            auto [s0, s1] = range_of(c);
            if (s0 >= s1) { vals[static_cast<std::size_t>(c)] = -1.0; continue; }  // blank
            double best = 0.0;
            int    best_s = s0;
            for (int s = s0; s < s1; ++s) {
                double raw = std::clamp<double>(data_[static_cast<std::size_t>(s)], 0.0, 1.0);
                if (raw >= best) { best = raw; best_s = s; }
            }
            double v = best;
            if (gamma_ != 1.0f) v = std::pow(v, static_cast<double>(gamma_));
            vals[static_cast<std::size_t>(c)] = v;
            peak_sample[static_cast<std::size_t>(c)] = best_s;
        }

        // Total inked height is rows_*8 eighths. Row 0 = top, rows_-1 = floor.
        const int total_eighths = rows_ * 8;

        std::vector<Element> out;
        out.reserve(static_cast<std::size_t>(rows_));
        for (int r = 0; r < rows_; ++r) {
            std::string content;
            std::vector<StyledRun> runs;
            content.reserve(static_cast<std::size_t>(cells_) * 3);
            // Eighths that belong to THIS row: rows count from the top, so the
            // floor row (r=rows_-1) covers eighths [0,8), the row above [8,16)…
            const int row_from_floor = rows_ - 1 - r;
            const int lo = row_from_floor * 8;       // eighths at/below this row's floor

            for (int c = 0; c < cells_; ++c) {
                const double v = vals[static_cast<std::size_t>(c)];
                if (v < 0.0) { content += ' '; continue; }   // pad column
                const int h = static_cast<int>(std::lround(v * total_eighths));
                int cell_eighths = std::clamp(h - lo, 0, 8);
                std::size_t off = content.size();
                content += kBar[cell_eighths];
                if (cell_eighths > 0) {
                    // Per-sample color wins when supplied; else fixed color;
                    // else the per-value load gradient. Tint by the sample
                    // that supplied this column's peak, so the color reflects
                    // the burst you actually see.
                    Color cc;
                    const int s = peak_sample[static_cast<std::size_t>(c)];
                    if (!col_colors_.empty() && s >= 0 &&
                        s < static_cast<int>(col_colors_.size())) {
                        cc = col_colors_[static_cast<std::size_t>(s)];
                    } else {
                        cc = color_ ? *color_ : load_color(v);
                    }
                    runs.push_back({off, content.size() - off, Style{}.with_fg(cc)});
                }
            }
            out.push_back(Element{TextElement{
                .content = std::move(content),
                .style   = {},
                .wrap    = TextWrap::NoWrap,
                .runs    = std::move(runs),
            }});
        }
        return maya::dsl::v(out).build();
    }
};

}  // namespace rockbottom::ui
