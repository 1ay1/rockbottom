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

public:
    BarChart(const float* data, int len) : len_(std::max(0, len)) {
        data_.assign(data, data + len_);
    }

    BarChart& cells(int n)         { cells_ = n; return *this; }   // <=0 → fill
    BarChart& rows(int n)          { rows_ = std::max(1, n); return *this; }
    BarChart& color(maya::Color c) { color_ = c; return *this; }
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

        const int start = len_ > cells_ ? len_ - cells_ : 0;
        const int shown = len_ - start;
        const int pad   = std::max(0, cells_ - shown);

        // Precompute, per column, the value fraction (curved) and its color.
        std::vector<double> vals(static_cast<std::size_t>(cells_), 0.0);
        for (int c = 0; c < cells_; ++c) {
            const int si = c - pad;                 // sample index within window
            if (si < 0) { vals[static_cast<std::size_t>(c)] = -1.0; continue; }  // blank pad
            double v = std::clamp<double>(data_[static_cast<std::size_t>(start + si)], 0.0, 1.0);
            if (gamma_ != 1.0f) v = std::pow(v, static_cast<double>(gamma_));
            vals[static_cast<std::size_t>(c)] = v;
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
                    const Color cc = color_ ? *color_ : load_color(v);
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
