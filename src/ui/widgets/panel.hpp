// widgets/panel.hpp — bottom::ui::Panel, the house panel chrome.
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

namespace bottom::ui {

class Panel {
    std::string glyph_, title_, chip_;
    maya::Color accent_;
    float grow_ = 0;
    double tint_ = 0.45;   // how much accent bleeds into the border line

public:
    Panel(std::string glyph, std::string title, maya::Color accent)
        : glyph_(std::move(glyph)), title_(std::move(title)), accent_(accent) {}

    Panel& grow(float g)              { grow_ = g; return *this; }
    Panel& chip(std::string c)        { chip_ = std::move(c); return *this; }
    Panel& tint(double t)             { tint_ = t; return *this; }

    [[nodiscard]] maya::Element operator()(std::vector<maya::Element> children) const {
        using namespace maya;

        auto b = dsl::vstack();   // keep the builder alive; methods return refs
        b.border(BorderStyle::Round)
         .border_color(mix(pal::border, accent_, tint_))
         .border_text(" " + glyph_ + " " + title_ + " ", BorderTextPos::Top)
         .padding(0, 1, 0, 1);
        if (!chip_.empty())
            b.border_text_end(" " + chip_ + " ", BorderTextPos::Top);

        Element body = b(std::move(children));
        if (grow_ > 0) return (std::move(body) | dsl::grow(grow_)).build();
        return body;
    }
};

}  // namespace bottom::ui
