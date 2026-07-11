// widgets/nice_menu.hpp — the renice picker (change scheduling priority).
//
// A compact centered dial. ←/→ (or ↑↓) nudge the nice value across the full
// -20…+19 range; a live scale shows where you are between "hog the CPU" and
// "yield to everything". Enter applies via setpriority(2); Esc backs out.
//
// Nice semantics read backwards to newcomers, so the UI spells it out:
//   -20 = highest priority (greedy)   0 = normal   +19 = lowest (nicest)

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"
#include "panel.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace rockbottom::ui {

class NiceMenu {
    int width_  = 100;
    int height_ = 40;
    std::string target_;
    int cur_ = 0;   // current nice
    int val_ = 0;   // dialed value

public:
    NiceMenu(int w, int h, std::string target, int cur, int val)
        : width_(w), height_(h), target_(std::move(target)), cur_(cur), val_(val) {}

    operator maya::Element() const { return build(); }

private:
    static maya::Color band(int v) {
        // Greedy (negative) reads hot; nice (positive) reads calm/teal.
        if (v < 0)  return pal::hot;
        if (v == 0) return pal::label;
        return pal::teal;
    }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        const int card_w = std::clamp(width_ - 6, 40, 58);

        std::vector<Element> body;
        {
            std::string t = target_;
            const std::size_t cap = static_cast<std::size_t>(std::max(10, card_w - 12));
            if (t.size() > cap) { t.resize(cap - 1); t += "…"; }
            body.push_back((h(
                text("set priority for ") | nowrap | fgc(pal::dim),
                text(t) | nowrap | Bold | fgc(pal::label)
            )).build());
        }
        body.push_back(blank());

        // The big dialed value + a plain-language gloss.
        const char* gloss = val_ < 0 ? "greedy — competes hard for the CPU"
                          : val_ == 0 ? "normal scheduling priority"
                          : val_ < 10 ? "polite — yields to normal work"
                          :             "idle-ish — runs only when nothing else wants the CPU";
        body.push_back((h(
            text("  nice ") | nowrap | fgc(pal::dim),
            text((val_ > 0 ? "+" : "") + std::to_string(val_)) | nowrap | Bold | fgc(band(val_)) | width(6),
            text(cur_ != val_ ? "(was " + std::to_string(cur_) + ")  " : "") | nowrap | fgc(pal::faint)
        )).build());
        body.push_back((text("  " + std::string(gloss)) | nowrap | fgc(pal::label)).build());
        body.push_back(blank());

        // A -20…+19 scale bar with the cursor riding it.
        {
            const int lo = -20, hi = 19, span = hi - lo;      // 39
            const int cells = std::clamp(card_w - 6, 20, 40);
            const int pos = (val_ - lo) * (cells - 1) / span;
            std::string bar;
            for (int i = 0; i < cells; ++i) bar += (i == pos) ? "●" : "─";
            std::vector<StyledRun> runs;
            runs.push_back({0, bar.size(), Style{}.with_fg(mix(pal::border, band(val_), 0.5))});
            // Tint the cursor cell in the band color.
            // (runs are byte offsets; ● is 3 bytes, find its byte start.)
            std::size_t bytepos = static_cast<std::size_t>(pos) * 3;
            runs.push_back({bytepos, 3, Style{}.with_bold().with_fg(band(val_))});
            body.push_back((h(
                text("  -20 ") | nowrap | fgc(pal::faint),
                Element{TextElement{.content = std::move(bar), .style = {},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(runs)}},
                text(" +19") | nowrap | fgc(pal::faint)
            )).build());
        }
        body.push_back(blank());
        body.push_back((h(
            text("  ←→ / ↑↓", Style{}.with_fg(pal::text).with_bold()) | nowrap | width(11),
            text("adjust   ", Style{}.with_fg(pal::dim)) | nowrap,
            text("enter", Style{}.with_fg(pal::text).with_bold()) | nowrap | width(7),
            text("apply   ", Style{}.with_fg(pal::dim)) | nowrap,
            text("esc", Style{}.with_fg(pal::text).with_bold()) | nowrap | width(5),
            text("cancel", Style{}.with_fg(pal::dim)) | nowrap
        )).build());

        Element card = Panel("⚙", "RENICE", pal::teal)({v(std::move(body))});
        return (v((std::move(card) | width(card_w)))
                | align(Align::Center) | justify(Justify::Center)
                | grow(1) | padding(1)).build();
    }
};

}  // namespace rockbottom::ui
