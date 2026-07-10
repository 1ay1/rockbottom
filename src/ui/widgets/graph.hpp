// widgets/graph.hpp — rockbottom::ui::Graph, a braille line + filled-area chart.
//
// Renders a history ring on a fixed 0..1 (0-100%) scale using braille dots
// (U+2800, 2x4 sub-cell resolution) for a crisp continuous trace, backed by
// a checker-dithered, depth-faded area fill — a translucent mountain — and
// faint dotted gridlines at 25/50/75% in the empty sky above it. The line
// color follows the load gradient at each column, so a spike glows amber/red.
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

namespace rockbottom::ui {

class Graph {
    const float* data_ = nullptr;
    int len_ = 0;
    int cells_ = 40;
    int rows_ = 4;
    std::optional<maya::Color> color_;   // nullopt → gradient by latest value
    // Optional second series drawn as a plain line (no fill glow) on the same
    // grid — e.g. RAM over the CPU graph, VRAM over the GPU graph.
    const float* overlay_ = nullptr;
    int overlay_len_ = 0;
    maya::Color overlay_color_ = pal::mem_ac;

public:
    Graph(const float* data, int len) : data_(data), len_(std::max(0, len)) {}

    Graph& cells(int n)         { cells_ = n; return *this; }   // <=0 → fill
    Graph& rows(int n)          { rows_ = std::max(1, n); return *this; }
    Graph& color(maya::Color c) { color_ = c; return *this; }
    Graph& fill()               { cells_ = 0; return *this; }
    // Overlay a second history series (drawn as a thin line in `c`).
    Graph& overlay(const float* data, int len, maya::Color c) {
        overlay_ = data; overlay_len_ = std::max(0, len); overlay_color_ = c; return *this;
    }

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        if (cells_ <= 0) {
            Graph self = *this;
            return Element{ComponentElement{
                .render = [self](int w, int) -> Element {
                    Graph g = self;
                    g.cells_ = std::max(1, w);
                    return g.build_fixed();
                },
            }};
        }
        return build_fixed();
    }

    [[nodiscard]] maya::Element build_fixed() const {
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

        // Gridline dot-rows at 25/50/75% — faint dotted guides that keep an
        // idle graph composed instead of a black void (dashboard-example
        // idiom). Drawn only where no data inks the cell.
        auto is_grid = [&](int gy, int gx) {
            if (gh < 8) return false;
            const int q1 = gh / 4, q2 = gh / 2, q3 = 3 * gh / 4;
            return (gy == q1 || gy == q2 || gy == q3) && (gx % 4 == 0);
        };

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

        // Optional overlay series: its own dot-row line on the same grid.
        const bool has_overlay = overlay_ && overlay_len_ > 0;
        std::vector<int> oline;
        if (has_overlay) {
            const int ostart = overlay_len_ > cells_ * 2 ? overlay_len_ - cells_ * 2 : 0;
            auto osample = [&](int gx) -> double {
                if (overlay_len_ - ostart <= 0) return 0.0;
                double t = static_cast<double>(gx) / std::max(1, gw - 1);
                double fi = t * (overlay_len_ - ostart - 1);
                int lo = static_cast<int>(fi);
                int hi = std::min(lo + 1, overlay_len_ - ostart - 1);
                double fr = fi - lo;
                double a = overlay_[ostart + lo], b = overlay_[ostart + hi];
                return std::clamp(a * (1 - fr) + b * fr, 0.0, 1.0);
            };
            oline.resize(static_cast<std::size_t>(gw));
            for (int gx = 0; gx < gw; ++gx) {
                double v = osample(gx);
                int gy = gh - 1 - static_cast<int>(std::lround(v * (gh - 1)));
                oline[static_cast<std::size_t>(gx)] = std::clamp(gy, 0, gh - 1);
            }
        }

        const Color lc = color_ ? *color_ : load_color(latest);

        std::vector<Element> out;
        out.reserve(static_cast<std::size_t>(rows_));

        for (int r = 0; r < rows_; ++r) {
            std::string content;
            std::vector<StyledRun> runs;
            content.reserve(static_cast<std::size_t>(cells_) * 3);

            for (int c = 0; c < cells_; ++c) {
                uint8_t line_bits = 0;
                uint8_t fill_bits = 0;
                uint8_t over_bits = 0;
                uint8_t grid_bits = 0;
                for (int dc = 0; dc < 2; ++dc) {
                    int gx = c * 2 + dc;
                    int ly = line[static_cast<std::size_t>(gx)];
                    int oy = has_overlay ? oline[static_cast<std::size_t>(gx)] : -1;
                    for (int dr = 0; dr < 4; ++dr) {
                        int gy = r * 4 + dr;
                        if (gy == ly)      line_bits |= kDot[dr][dc];
                        // Half-density dither: only every other dot (checker
                        // parity) inks, so the mountain reads as translucent
                        // shading instead of a solid dot wall.
                        else if (gy > ly && ((gx + gy) & 1)) fill_bits |= kDot[dr][dc];
                        // The overlay is a LINE ONLY — a second dithered
                        // mountain over the primary's fill reads as noise
                        // (two checker patterns interfere into a moiré band).
                        if (has_overlay && gy == oy) over_bits |= kDot[dr][dc];
                        if (gy < ly && (!has_overlay || gy < oy) && is_grid(gy, gx))
                            grid_bits |= kDot[dr][dc];
                    }
                }
                // Combine everything into one braille glyph. A glyph carries
                // ONE color, so pick by priority: where BOTH crests share the
                // cell, blend the hues (an idle primary hugging the floor
                // must never be fully painted over by the overlay); else
                // whichever crest is present > primary fill.
                const uint8_t bits = line_bits | fill_bits | over_bits | grid_bits;
                if (bits) {
                    std::size_t off = content.size();
                    utf8(U'\u2800' + bits, content);
                    const double depth = rows_ > 1 ? static_cast<double>(r) / (rows_ - 1) : 1.0;
                    const Color bright = color_ ? *color_
                        : load_color(1.0 - line[static_cast<std::size_t>(c * 2)] / double(std::max(1, gh - 1)));
                    Color cc;
                    if (line_bits && over_bits) {
                        cc = mix(bright, overlay_color_, 0.5);
                    } else if (over_bits) {
                        cc = overlay_color_;
                    } else if (line_bits) {
                        cc = bright;
                    } else if (fill_bits) {
                        // Depth fade: rows further from the top are dimmer.
                        cc = mix(bright, pal::bg_panel, 0.45 + 0.25 * depth);
                    } else {
                        cc = pal::track;   // gridline dots
                    }
                    runs.push_back({off, content.size() - off, Style{}.with_fg(cc)});
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

        (void)lc;
        return maya::dsl::v(out).build();
    }
};

}  // namespace rockbottom::ui
