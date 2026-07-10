// widgets/graph.hpp — bottom::ui::Graph, a braille line + filled-area chart.
//
// Renders a history ring on a fixed 0..1 (0-100%) scale using braille dots
// (U+2800, 2x4 sub-cell resolution) for a crisp continuous trace, backed by
// a dim filled area so the "mountain" reads at a glance while the bright
// leading line shows the exact shape. The line color follows the load
// gradient of the *latest* sample, so a spiking graph glows amber/red.
//
//   Graph{hist.data(), len}.cells(46).rows(4)
//
// Unlike a block chart there is no silence rule: a flat-idle series draws a
// clean line hugging the floor, not a field of gaps.

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bottom::ui {

class Graph {
    const float* data_ = nullptr;
    int len_ = 0;
    int cells_ = 40;
    int rows_ = 4;
    std::optional<maya::Color> color_;   // nullopt → gradient by latest value

public:
    Graph(const float* data, int len) : data_(data), len_(std::max(0, len)) {}

    Graph& cells(int n)         { cells_ = std::max(1, n); return *this; }
    Graph& rows(int n)          { rows_ = std::max(1, n); return *this; }
    Graph& color(maya::Color c) { color_ = c; return *this; }

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;

        // Braille dot bit for (dot_row 0..3, dot_col 0..1).
        static constexpr uint8_t kDot[4][2] = {
            {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80},
        };
        auto utf8 = [](char32_t cp, std::string& out) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        };

        const int gw = cells_ * 2;          // dot columns
        const int gh = rows_ * 4;           // dot rows
        const int start = len_ > cells_ * 2 ? len_ - cells_ * 2 : 0;

        auto sample = [&](int gx) -> double {     // value at dot-column gx
            if (len_ - start <= 0) return 0.0;
            double t = static_cast<double>(gx) / std::max(1, gw - 1);
            double fi = t * (len_ - start - 1);
            int lo = static_cast<int>(fi);
            int hi = std::min(lo + 1, len_ - start - 1);
            double fr = fi - lo;
            double a = data_[start + lo], b = data_[start + hi];
            return std::clamp(a * (1 - fr) + b * fr, 0.0, 1.0);
        };

        // line[gx] = dot-row (0=top) of the trace; fill everything below.
        std::vector<int> line(static_cast<std::size_t>(gw));
        double latest = len_ > 0 ? std::clamp<double>(data_[len_ - 1], 0.0, 1.0) : 0.0;
        for (int gx = 0; gx < gw; ++gx) {
            double v = sample(gx);
            int gy = gh - 1 - static_cast<int>(std::lround(v * (gh - 1)));
            line[static_cast<std::size_t>(gx)] = std::clamp(gy, 0, gh - 1);
        }

        const Color lc = color_ ? *color_ : load_color(latest);
        const Color fc = mix(pal::track, lc, 0.55);   // dim area under the line

        std::vector<Element> out;
        out.reserve(static_cast<std::size_t>(rows_));

        for (int r = 0; r < rows_; ++r) {
            std::string content;
            std::vector<StyledRun> runs;
            content.reserve(static_cast<std::size_t>(cells_) * 3);

            for (int c = 0; c < cells_; ++c) {
                uint8_t line_bits = 0, fill_bits = 0;
                for (int dc = 0; dc < 2; ++dc) {
                    int gx = c * 2 + dc;
                    int ly = line[static_cast<std::size_t>(gx)];
                    for (int dr = 0; dr < 4; ++dr) {
                        int gy = r * 4 + dr;
                        if (gy == ly)      line_bits |= kDot[dr][dc];
                        else if (gy > ly)  fill_bits |= kDot[dr][dc];
                    }
                }
                // The trace glyph wins its cell; the pure-fill glyph is dim.
                if (line_bits) {
                    std::size_t off = content.size();
                    utf8(U'\u2800' + (line_bits | fill_bits), content);
                    runs.push_back({off, content.size() - off, Style{}.with_fg(lc)});
                } else if (fill_bits) {
                    std::size_t off = content.size();
                    utf8(U'\u2800' + fill_bits, content);
                    runs.push_back({off, content.size() - off, Style{}.with_fg(fc)});
                } else {
                    content += ' ';
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

}  // namespace bottom::ui
