// widgets/meter.hpp — bottom::ui::Meter, a maya-idiomatic gradient bar meter.
//
// The maya way: a widget is a small class satisfying the Node concept
// (build() -> Element, operator Element), configured by value, styled with
// StyledRun spans inside ONE TextElement — the renderer paints each run
// with its own color, so a 24-cell gradient bar costs one element, not 24.
//
//   Meter{0.72}.width(24)                    → gradient green→…→red fill
//   Meter{0.31}.width(10).color(pal::teal)   → flat accent fill
//
// The fill uses eighth-block partials (▏▎▍▌▋▊▉█) for sub-cell smoothness.
// The empty remainder is a solid dark GROOVE (bg-painted cells), so the bar
// reads as one object — a pill with a recessed track — instead of the fill
// drowning in a field of dots.

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace bottom::ui {

class Meter {
    double value_ = 0;                       // 0..1
    int    width_ = 16;
    std::optional<maya::Color> color_;       // nullopt → load gradient
    maya::Color track_ = pal::track;

public:
    constexpr explicit Meter(double value) : value_(std::clamp(value, 0.0, 1.0)) {}

    Meter& width(int w)              { width_ = w; return *this; }   // <=0 → fill
    Meter& color(maya::Color c)      { color_ = c; return *this; }
    Meter& track(maya::Color c)      { track_ = c; return *this; }
    Meter& fill()                    { width_ = 0; return *this; }

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        // Fill mode: size to whatever width the row hands us. Pair with
        // `| grow(1)` so maya's box layout allocates the leftover space.
        if (width_ <= 0) {
            Meter self = *this;
            return maya::Element{maya::ComponentElement{
                .render = [self](int w, int) -> maya::Element {
                    Meter m = self;
                    m.width_ = std::max(1, w);
                    return m.build_fixed();
                },
            }};
        }
        return build_fixed();
    }

    [[nodiscard]] maya::Element build_fixed() const {
        static constexpr const char* kEighths[] =
            {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
        static constexpr const char* kFull = "█";

        const double cells = value_ * width_;
        const int full = static_cast<int>(cells);
        const int frac8 = std::clamp(static_cast<int>((cells - full) * 8.0), 0, 7);

        std::string content;
        std::vector<maya::StyledRun> runs;
        content.reserve(static_cast<std::size_t>(width_) * 3);

        // Filled cells — each cell gets the gradient color of ITS position,
        // so the bar itself narrates severity as it grows.
        for (int i = 0; i < full && i < width_; ++i) {
            maya::Color c = color_ ? *color_
                                   : load_color((i + 0.5) / width_);
            std::size_t off = content.size();
            content += kFull;
            runs.push_back({off, content.size() - off, maya::Style{}.with_fg(c)});
        }

        // Partial cell: eighth-block fill over the groove color, so the cell's
        // unfilled remainder shows the track — a seamless boundary.
        int used = full;
        if (full < width_ && frac8 > 0) {
            maya::Color c = color_ ? *color_ : load_color((full + 0.5) / width_);
            std::size_t off = content.size();
            content += kEighths[frac8];
            runs.push_back({off, content.size() - off,
                            maya::Style{}.with_fg(c).with_bg(track_)});
            ++used;
        }

        // Groove remainder: one run of bg-painted spaces — a quiet solid slab.
        if (used < width_) {
            std::size_t off = content.size();
            for (int i = used; i < width_; ++i) content += ' ';
            runs.push_back({off, content.size() - off, maya::Style{}.with_bg(track_)});
        }

        return maya::Element{maya::TextElement{
            .content = std::move(content),
            .style   = {},
            .wrap    = maya::TextWrap::NoWrap,
            .runs    = std::move(runs),
        }};
    }
};

}  // namespace bottom::ui
