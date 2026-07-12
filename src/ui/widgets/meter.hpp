// widgets/meter.hpp — rockbottom::ui::Meter, a maya-idiomatic gradient bar meter.
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
#include <maya/widget/table.hpp>

#include "../theme.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace rockbottom::ui {

class Meter {
    double value_ = 0;                       // 0..1
    int    width_ = 16;
    std::optional<maya::Color> color_;       // nullopt → load gradient
    maya::Color track_ = pal::track;
    bool groove_ = true;                     // paint the empty remainder slab

public:
    constexpr explicit Meter(double value) : value_(std::clamp(value, 0.0, 1.0)) {}

    Meter& width(int w)              { width_ = w; return *this; }   // <=0 → fill
    Meter& color(maya::Color c)      { color_ = c; return *this; }
    Meter& track(maya::Color c)      { track_ = c; return *this; }
    Meter& fill()                    { width_ = 0; return *this; }
    Meter& groove(bool on)           { groove_ = on; return *this; }   // off = no bg slab

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        // Fill mode: size to whatever width the row hands us. Pair with
        // `| grow(1)` so maya's box layout allocates the leftover space.
        // measure = 1x1: a fill meter has no natural width of its own — it
        // takes what the row gives it. Without this the ComponentElement
        // default measure ({max_width, 1}) claims the WHOLE row as this
        // cell's flex basis, over-subscribing the h-stack so yoga shrinks
        // the fixed-width label/value siblings (the "io pressur" bug class).
        if (width_ <= 0) {
            Meter self = *this;
            maya::ComponentElement ce{
                .render = [self](int w, int) -> maya::Element {
                    Meter m = self;
                    m.width_ = std::max(1, w);
                    return m.build_fixed();
                },
                .measure = [](int) -> maya::Size {
                    return {maya::Columns{1}, maya::Rows{1}};
                },
            };
            // Grow on the COMPONENT itself (maya fill() contract): a piped
            // `| grow(1)` wraps in a box — the box grows but a measured leaf
            // inside it keeps natural width unless IT also claims the space.
            ce.layout.grow = 1.0f;
            return maya::Element{std::move(ce)};
        }
        return build_fixed();
    }

    // A maya::Table cell: the bar is painted at the column's SOLVED
    // width every frame (TableCell::dyn), so it breathes with the
    // column plan exactly like the fill-mode element does with its box.
    [[nodiscard]] maya::TableCell table_cell() const {
        Meter self = *this;
        return maya::TableCell::dyn([self](int w) {
            Meter m = self;
            m.width_ = std::max(1, w);
            auto p = m.paint();
            return maya::TableCell{std::move(p.content), std::move(p.runs)};
        });
    }

    [[nodiscard]] maya::Element build_fixed() const {
        auto p = paint();
        return maya::Element{maya::TextElement{
            .content = std::move(p.content),
            .style   = {},
            .wrap    = maya::TextWrap::NoWrap,
            .runs    = std::move(p.runs),
        }};
    }

private:
    struct Painted {
        std::string content;
        std::vector<maya::StyledRun> runs;
    };

    [[nodiscard]] Painted paint() const {
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
        // so the bar itself narrates severity as it grows. The leading cell
        // is lifted toward white — a glossy tip that marks the live edge.
        const int tip = (full < width_ && frac8 > 0) ? full : full - 1;
        for (int i = 0; i < full && i < width_; ++i) {
            maya::Color c = color_ ? *color_
                                   : load_color((i + 0.5) / width_);
            if (i == tip) c = mix(c, pal::white, 0.35);
            std::size_t off = content.size();
            content += kFull;
            runs.push_back({off, content.size() - off, maya::Style{}.with_fg(c)});
        }

        // Partial cell: eighth-block fill over the groove color, so the cell's
        // unfilled remainder shows the track — a seamless boundary. As the
        // leading edge it carries the glossy tip.
        int used = full;
        if (full < width_ && frac8 > 0) {
            maya::Color c = color_ ? *color_ : load_color((full + 0.5) / width_);
            c = mix(c, pal::white, 0.35);
            std::size_t off = content.size();
            content += kEighths[frac8];
            auto st = maya::Style{}.with_fg(c);
            if (groove_) st = st.with_bg(track_);
            runs.push_back({off, content.size() - off, st});
            ++used;
        }

        // Groove remainder: one run of bg-painted spaces — a quiet solid slab.
        // With the groove off the empty cells are plain blanks, so an idle row
        // shows nothing but its fill (no dark rectangle to read as noise).
        if (used < width_) {
            std::size_t off = content.size();
            for (int i = used; i < width_; ++i) content += ' ';
            if (groove_)
                runs.push_back({off, content.size() - off, maya::Style{}.with_bg(track_)});
        }

        return {std::move(content), std::move(runs)};
    }
};

}  // namespace rockbottom::ui
