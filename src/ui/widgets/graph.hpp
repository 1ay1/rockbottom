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
#include "../../core/units.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
    // Perceptual height curve. Byte-rate traffic is bursty: one old spike
    // sets the peak and every quieter sample crushes to a floor line. A sqrt
    // curve lifts low values off the floor so a 2 KB/s trickle still reads as
    // a shape, while the peak stays pinned at the top. Off (=1) for percent
    // graphs (cpu/mem/gpu) whose 0..1 axis is already the real quantity.
    float gamma_ = 1.0f;
    // Fill style below the trace. Full = the checker-dither mountain (default,
    // historic). Light = a single dot-column rail every few cells (a faint
    // "rain" under the line, far less busy). None = a bare line, no fill.
    enum class Fill { Full, Light, None };
    Fill fillmode_ = Fill::Full;

public:
    Graph(const float* data, int len) : data_(data), len_(std::max(0, len)) {}

    Graph& cells(int n)         { cells_ = n; return *this; }   // <=0 → fill
    Graph& rows(int n)          { rows_ = std::max(1, n); return *this; }
    Graph& color(maya::Color c) { color_ = c; return *this; }
    Graph& fill()               { cells_ = 0; return *this; }
    // gamma<1 compresses the top / expands the bottom (0.5 = sqrt).
    Graph& gamma(float g)       { gamma_ = std::max(0.05f, g); return *this; }
    // A bare line, no area fill — the cleanest trace, reads as a curve.
    Graph& line_only()          { fillmode_ = Fill::None; return *this; }
    // A faint sparse rain under the line instead of the solid dither wall.
    Graph& light_fill()         { fillmode_ = Fill::Light; return *this; }
    // Overlay a second history series (drawn as a thin line in `c`).
    Graph& overlay(const float* data, int len, maya::Color c) {
        overlay_ = data; overlay_len_ = std::max(0, len); overlay_color_ = c; return *this;
    }

    // Apply the perceptual curve to a 0..1 fraction (identity when gamma==1).
    [[nodiscard]] double curve(double v) const {
        if (gamma_ == 1.0f) return v;
        return std::pow(std::clamp(v, 0.0, 1.0), static_cast<double>(gamma_));
    }

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        if (cells_ <= 0) {
            Graph self = *this;
            const int nat_rows = rows_;
            ComponentElement ce{
                .render = [self](int w, int) -> Element {
                    Graph g = self;
                    g.cells_ = std::max(1, w);
                    return g.build_fixed();
                },
                // A fill graph has no natural width (it stretches to the
                // slot) but a REAL height (rows_). Without this the default
                // auto-measure claims the whole row width as flex basis and
                // yoga shrinks the fixed-width siblings (y-axis, stat card).
                .measure = [nat_rows](int) -> Size {
                    return {Columns{1}, Rows{nat_rows}};
                },
            };
            // Grow on the COMPONENT (maya fill() contract): a piped grow
            // wraps in a box that grows while the measured leaf inside
            // keeps its 1-cell natural width.
            ce.layout.grow = 1.0f;
            return Element{std::move(ce)};
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

        // Gridline dot-rows — faint dotted guides that keep an idle graph
        // composed instead of a black void (dashboard-example idiom). Drawn
        // only where no data inks the cell. Kept SPARSE on purpose: one dot
        // every 4 character-cells (gx % 8) so the sky reads as a light ruled
        // guide, not a solid field of dots that drowns a floor-hugging trace.
        // A short graph gets the 50% midline only; a tall one adds 25/75%.
        auto is_grid = [&](int gy, int gx) {
            if (gh < 8) return false;
            if (fillmode_ == Fill::None) return false;   // a bare line stands alone
            if (gx % 8 != 0) return false;
            const int q2 = gh / 2;
            if (gy == q2) return true;
            if (gh >= 16) {
                const int q1 = gh / 4, q3 = 3 * gh / 4;
                return gy == q1 || gy == q3;
            }
            return false;
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
        // A value maps into [floor_gy .. 0]: even a dead-flat 0 sits `lift`
        // dot-rows above the very bottom so an idle series still draws a
        // visible baseline trace (with a thin fill band beneath it) rather
        // than collapsing to a 1-dot sliver on the floor.
        const int lift = gh >= 8 ? 2 : 1;
        const int floor_gy = gh - 1 - lift;
        std::vector<int> line(static_cast<std::size_t>(gw));
        double latest = len_ > 0 ? std::clamp<double>(data_[len_ - 1], 0.0, 1.0) : 0.0;
        for (int gx = 0; gx < gw; ++gx) {
            double v = curve(sample(gx));
            int gy = floor_gy - static_cast<int>(std::lround(v * floor_gy));
            line[static_cast<std::size_t>(gx)] = std::clamp(gy, 0, gh - 1);
        }

        // Connected stroke: at each dot-column the trace occupies not just its
        // own dot-row but the whole vertical span up to the MIDPOINT of its
        // neighbours — so a bumpy series reads as one continuous curve instead
        // of a field of disconnected dots (the confetti look). stroke_lo/hi
        // bracket the inked dot-rows for column gx.
        auto stroke_span = [&](int gx, int& lo, int& hi) {
            const int y  = line[static_cast<std::size_t>(gx)];
            lo = y; hi = y;
            if (gx > 0) {
                const int yl = line[static_cast<std::size_t>(gx - 1)];
                const int mid = (y + yl) / 2;   // meet the left neighbour halfway
                lo = std::min(lo, std::min(y, mid));
                hi = std::max(hi, std::max(y, mid));
            }
            if (gx < gw - 1) {
                const int yr = line[static_cast<std::size_t>(gx + 1)];
                const int mid = (y + yr + 1) / 2; // meet the right neighbour halfway
                lo = std::min(lo, std::min(y, mid));
                hi = std::max(hi, std::max(y, mid));
            }
        };

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
                double v = curve(osample(gx));
                int gy = floor_gy - static_cast<int>(std::lround(v * floor_gy));
                oline[static_cast<std::size_t>(gx)] = std::clamp(gy, 0, gh - 1);
            }
        }
        // Same connected-stroke bracket for the overlay line.
        auto ostroke_span = [&](int gx, int& lo, int& hi) {
            const int y = oline[static_cast<std::size_t>(gx)];
            lo = y; hi = y;
            if (gx > 0) {
                const int mid = (y + oline[static_cast<std::size_t>(gx - 1)]) / 2;
                lo = std::min(lo, std::min(y, mid)); hi = std::max(hi, std::max(y, mid));
            }
            if (gx < gw - 1) {
                const int mid = (y + oline[static_cast<std::size_t>(gx + 1)] + 1) / 2;
                lo = std::min(lo, std::min(y, mid)); hi = std::max(hi, std::max(y, mid));
            }
        };

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
                // Track the deepest fill dot-row in this cell so the area's
                // color can fade smoothly with real height, not just row index.
                int fill_gy_sum = 0, fill_gy_n = 0;
                for (int dc = 0; dc < 2; ++dc) {
                    int gx = c * 2 + dc;
                    int ly = line[static_cast<std::size_t>(gx)];
                    int slo, shi; stroke_span(gx, slo, shi);
                    int oy = has_overlay ? oline[static_cast<std::size_t>(gx)] : -1;
                    int olo = -1, ohi = -1;
                    if (has_overlay) ostroke_span(gx, olo, ohi);
                    for (int dr = 0; dr < 4; ++dr) {
                        int gy = r * 4 + dr;
                        // Connected trace: the whole bracket between this
                        // column and its neighbours' midpoints inks, so the
                        // curve is unbroken instead of a scatter of dots.
                        const bool on_line = gy >= slo && gy <= shi;
                        if (on_line) line_bits |= kDot[dr][dc];
                        // Solid area fill under the trace. A smooth mountain
                        // (every dot below the stroke), colored by a vertical
                        // gradient later. Fill::None = a bare line only.
                        else if (gy > shi && fillmode_ != Fill::None) {
                            fill_bits |= kDot[dr][dc];
                            fill_gy_sum += gy; ++fill_gy_n;
                        }
                        // Overlay is a connected LINE ONLY (no second fill —
                        // two areas interfere into visual noise).
                        if (has_overlay && gy >= olo && gy <= ohi)
                            over_bits |= kDot[dr][dc];
                        if (!on_line && gy < ly && (!has_overlay || gy < oy)
                            && is_grid(gy, gx))
                            grid_bits |= kDot[dr][dc];
                    }
                }
                // Combine everything into one braille glyph. A glyph carries
                // ONE color, so pick by priority: line > overlay > fill > grid.
                // Where the primary line and overlay share the cell, blend.
                const uint8_t bits = line_bits | fill_bits | over_bits | grid_bits;
                if (bits) {
                    std::size_t off = content.size();
                    utf8(U'\u2800' + bits, content);
                    // The trace hue tracks the value at THIS cell (gradient
                    // graphs glow amber/red at a spike); fixed-color graphs
                    // use their color throughout.
                    const Color bright = color_ ? *color_
                        : load_color(1.0 - line[static_cast<std::size_t>(c * 2)]
                                            / double(std::max(1, gh - 1)));
                    Color cc;
                    if (line_bits && over_bits) {
                        cc = mix(bright, overlay_color_, 0.5);
                    } else if (line_bits) {
                        // The crest edge is the brightest thing on the graph —
                        // a small lift makes the leading stroke glow above its
                        // own gradient body.
                        cc = brighten(bright);
                    } else if (over_bits) {
                        cc = overlay_color_;
                    } else if (fill_bits) {
                        // Smooth vertical gradient: the area is brightest just
                        // under the crest and fades toward the panel bg at the
                        // floor, so the mountain has real depth. frac 0=top
                        // dot-row, 1=floor.
                        const double avg_gy = fill_gy_n
                            ? static_cast<double>(fill_gy_sum) / fill_gy_n
                            : (r * 4 + 1.5);
                        const double frac = std::clamp(avg_gy / std::max(1, gh - 1), 0.0, 1.0);
                        cc = mix(bright, pal::bg_panel, 0.42 + 0.46 * frac);
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

// A labelled y-axis column for a graph of `rows` height. Draws a tick label
// at the top, bottom, and the quarter lines in between (so the scale reads
// 100 / 75 / 50 / 25 / 0, not just the two ends). `top_val` is the value at
// the crest; format() turns a fraction-of-top into its label (percent by
// default). The column is `w` cells wide, right-aligned, in faint ink so it
// frames the graph without competing with the trace.
inline maya::Element y_axis(int rows, double top_val = 100.0, int w = 4,
                           bool percent = true, float gamma = 1.0f) {
    using namespace maya; using namespace maya::dsl;
    std::vector<std::string> labels(static_cast<std::size_t>(std::max(1, rows)));

    auto fmt_val = [&](double frac) -> std::string {
        // frac is the fraction of GRAPH HEIGHT; invert the plot curve so the
        // label reads the real byte value that lands at that row (a sqrt plot
        // puts value^gamma at height frac → value = top * frac^(1/gamma)).
        const double vfrac = gamma == 1.0f
            ? frac
            : std::pow(std::clamp(frac, 0.0, 1.0), 1.0 / static_cast<double>(gamma));
        const double val = top_val * vfrac;
        char buf[16];
        if (percent) std::snprintf(buf, sizeof buf, "%d", static_cast<int>(std::lround(val)));
        else {
            std::string h = humanize_bytes(static_cast<std::uint64_t>(val));
            std::snprintf(buf, sizeof buf, "%s", h.c_str());
        }
        return buf;
    };

    if (rows >= 1) {
        // Assign each tick to exactly ONE row (the closest), so a tick can
        // never print on two adjacent rows. Tall graphs get 5 ticks
        // (100/75/50/25/0), short ones fall back to 3 (100/50/0).
        // Row 0 is the TOP (frac 1.0), row rows-1 is the floor (frac 0.0).
        const std::vector<double> ticks = rows >= 6
            ? std::vector<double>{1.0, 0.75, 0.5, 0.25, 0.0}
            : std::vector<double>{1.0, 0.5, 0.0};
        for (double t : ticks) {
            // frac = 1 - r/(rows-1)  →  r = (1 - frac) * (rows - 1)
            int r = rows <= 1 ? 0
                  : static_cast<int>(std::lround((1.0 - t) * (rows - 1)));
            r = std::clamp(r, 0, rows - 1);
            // First tick to claim a row wins; a later tick that maps to the
            // same row is simply dropped (prevents overlap on tiny graphs).
            if (labels[static_cast<std::size_t>(r)].empty())
                labels[static_cast<std::size_t>(r)] = fmt_val(t);
        }
    }

    std::vector<Element> col;
    for (int r = 0; r < rows; ++r)
        col.push_back((text(labels[static_cast<std::size_t>(r)]) | nowrap | fgc(pal::faint)
                       | width(w) | justify(Justify::End)).build());
    return (v(std::move(col)) | width(w)).build();
}

}  // namespace rockbottom::ui
