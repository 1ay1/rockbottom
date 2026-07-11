// widgets/panel.hpp — rockbottom::ui::Panel, the house panel chrome.
//
// Every section is a "card": rounded border tinted with the domain's accent
// hue, a glyph-tagged title on the top edge, and an optional right-aligned
// info chip (border_text_end) for at-a-glance stats like "12 cores" or "42°C".
//
//   Panel("◈", "CPU", pal::cpu_ac).chip("42°C")(std::move(rows))
//
// maya idiom: builder-style widget (like maya::detail::vstack) — configure by
// value, call with children to produce the Element. Border text inherits the
// border style, so the accent tint colors title + chip + frame coherently.

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"

#include <string>
#include <utility>
#include <vector>

namespace rockbottom::ui {

class Panel {
    std::string glyph_, title_, chip_;
    maya::Color accent_;
    float grow_ = 0;
    double tint_ = 0.25;   // how much accent bleeds into the border line

public:
    Panel(std::string glyph, std::string title, maya::Color accent)
        : glyph_(std::move(glyph)), title_(std::move(title)), accent_(accent) {}

    Panel& grow(float g)              { grow_ = g; return *this; }
    Panel& chip(std::string c)        { chip_ = std::move(c); return *this; }
    Panel& chip_if(bool on, std::string c) { if (on) chip_ = std::move(c); return *this; }
    Panel& tint(double t)             { tint_ = t; return *this; }

    [[nodiscard]] maya::Element operator()(std::vector<maya::Element> children) const {
        using namespace maya;

        // Width-aware: the chip rides the top border only when the panel is
        // wide enough to also show the title. Below that it's dropped so the
        // title never gets crowded out.
        auto make = [self = *this, kids = std::move(children)](bool with_chip) mutable {
            auto b = dsl::vstack();   // keep the builder alive; methods return refs
            b.border(BorderStyle::Round)
             .border_color(mix(pal::border, self.accent_, self.tint_))
             .border_text(" " + self.glyph_ + " " + self.title_ + " ", BorderTextPos::Top)
             .padding(0, 1, 0, 1);
            if (with_chip && !self.chip_.empty())
                b.border_text_end(" " + self.chip_ + " ", BorderTextPos::Top);
            Element body = b(std::move(kids));
            if (self.grow_ > 0) return (std::move(body) | dsl::grow(self.grow_)).build();
            return body;
        };

        // GROW mode: the panel fills its flex slot (its inner content, e.g. a
        // fill() graph, expands into the slack). The outermost element MUST
        // stay a BOX so `| grow` / `| hit` piped by the caller land on it
        // in-place (WrappedNode's as_box path) instead of wrapping it in a
        // non-growing box that would leave the panel parked at natural height
        // inside its taller slot — the "still space" gap. A growing panel is
        // by construction roomy, so the chip always fits; skip the
        // width-responsive chip-drop (that path returns a component, which
        // can't carry grow/hit in-place).
        if (grow_ > 0)
            return make(true);

        if (chip_.empty())
            return make(false);

        // Title + chip both want horizontal room; if the inner width can't
        // hold title+chip+margins, drop the chip.
        const int need = static_cast<int>(glyph_.size() ? 3 : 0)
                       + static_cast<int>(title_.size())
                       + static_cast<int>(chip_.size()) + 8;
        auto self = *this;
        return Element{ComponentElement{
            .render = [self, make, need](int w, int) mutable -> Element {
                return make(w >= need);
            },
        }};
    }
};

}  // namespace rockbottom::ui
