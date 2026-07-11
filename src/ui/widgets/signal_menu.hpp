// widgets/signal_menu.hpp — the "send a signal" picker (htop's F9 menu).
//
// A centered overlay listing the curated signal catalog. The number keys 1-9
// and ↑↓ move the highlight; Enter / y arms a PendingKill with the chosen
// signal, routing back through the ordinary confirm flow so ANY signal — not
// just TERM/KILL — gets the same "are you sure" guard and group semantics.

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"
#include "../state.hpp"
#include "panel.hpp"
#include "detail/common.hpp"

#include <string>
#include <vector>

namespace rockbottom::ui {

class SignalMenu {
    int width_  = 100;
    int height_ = 40;
    std::string target_;   // "name (pid)" or "N × name"
    int sel_    = 0;       // highlighted index into signal_catalog()

public:
    SignalMenu(int w, int h, std::string target, int sel)
        : width_(w), height_(h), target_(std::move(target)), sel_(sel) {}

    operator maya::Element() const { return build(); }

private:
    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        const auto& cat = signal_catalog();
        const int card_w = std::clamp(width_ - 6, 34, 54);

        std::vector<Element> body;
        {
            // Keep the header on one line inside the card.
            std::string t = target_;
            const std::size_t cap = static_cast<std::size_t>(std::max(10, card_w - 20));
            if (t.size() > cap) { t.resize(cap - 1); t += "…"; }
            body.push_back((h(
                text("send a signal to ") | nowrap | fgc(pal::dim),
                text(t) | nowrap | Bold | fgc(pal::label)
            )).build());
        }
        body.push_back(blank());

        for (std::size_t i = 0; i < cat.size(); ++i) {
            const SignalDef& s = cat[i];
            const bool on = static_cast<int>(i) == sel_;
            // A hotkey rides the first nine rows (1-9); the rest are ↑↓ only.
            const std::string hot = i < 9 ? std::to_string(i + 1) : " ";
            const Color ac  = pal::hot;
            const Color ink = on ? pal::white : pal::text;
            std::vector<Element> row;
            // Selection bar in the domain accent, help-pane idiom.
            row.push_back((text(on ? "▎" : " ") | nowrap | fgc(ac)).build());
            row.push_back((text(hot) | nowrap | Bold
                           | fgc(on ? ac : pal::faint) | width(2)).build());
            row.push_back((text(s.name) | nowrap | Bold | fgc(ink) | width(10)).build());
            row.push_back((text(std::to_string(s.num)) | nowrap
                           | fgc(pal::dim) | width(4) | justify(Justify::End)).build());
            row.push_back((text("  " + std::string(s.gloss)) | nowrap
                           | fgc(on ? pal::label : pal::dim)).build());
            Element r = h(std::move(row)) | gap(0);
            if (on) r = std::move(r) | bgc(pal::track);
            body.push_back(r.build());
        }

        body.push_back(blank());
        body.push_back((h(
            text("  ") | nowrap,
            text("1-9 / ↑↓", Style{}.with_fg(pal::text).with_bold()) | nowrap | width(11),
            text("pick   ", Style{}.with_fg(pal::dim)) | nowrap,
            text("enter / y", Style{}.with_fg(pal::text).with_bold()) | nowrap | width(11),
            text("send   ", Style{}.with_fg(pal::dim)) | nowrap,
            text("esc", Style{}.with_fg(pal::text).with_bold()) | nowrap | width(5),
            text("cancel", Style{}.with_fg(pal::dim)) | nowrap
        ) | gap(0)).build());

        Element card = Panel("⚑", "SEND SIGNAL", pal::hot)({v(std::move(body))});
        return (v((std::move(card) | width(card_w)))
                | align(Align::Center) | justify(Justify::Center)
                | grow(1) | padding(1)).build();
    }
};

}  // namespace rockbottom::ui
