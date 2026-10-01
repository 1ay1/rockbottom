// widgets/signal_menu.hpp — the "send a signal" picker (htop's F9 menu).
//
// A centered overlay listing the curated signal catalog. Every row has a
// hotkey (1-9, then 0, a, b for the last three), rows are clickable, and
// ↑↓ move the highlight; Enter / y arms a PendingKill with the chosen
// signal, routing back through the ordinary confirm flow so ANY signal — not
// just TERM/KILL — gets the same "are you sure" guard and group semantics.

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"
#include "../state.hpp"
#include "hit_ids.hpp"
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
        // Min 46 so the widest gloss ("hang up / reload") and the hint row fit
        // without clipping mid-word; never exceed the terminal (clamp upper to
        // width_-4 so a 44-col terminal still shows a bordered, padded card).
        const int card_w = std::clamp(width_ - 6, std::min(46, std::max(30, width_ - 4)), 54);

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
            // EVERY row carries a hotkey. The first nine are 1-9, then 0 for
            // the tenth and a/b for the eleventh and twelfth. It used to stop
            // at nine and print a blank for the rest, which read as "these
            // three are not selectable" — SIGABRT, SIGWINCH and SIGTSTP were
            // reachable only by arrow key, in the one dialog where you most
            // want to hit the thing you meant on the first try.
            const std::string hot =
                i < 9  ? std::to_string(i + 1)
              : i == 9 ? std::string("0")
              : i == 10 ? std::string("a")
              : i == 11 ? std::string("b")
                        : std::string(" ");
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
            // Gloss fills the remaining card width; truncate (display-safe) so
            // a long one can't overrun the panel border into a clipped stub.
            // Fixed cells before it: bar(1)+hot(2)+name(10)+num(4)+"  "(2) = 19.
            const int gloss_w = std::max(6, card_w - 2 /*pad*/ - 19);
            row.push_back((text("  " + std::string(truncate_end(s.gloss, gloss_w))) | nowrap
                           | fgc(on ? pal::label : pal::dim)).build());
            // Clickable, like every other list in the app. This widget had no
            // hit regions at all, so the mouse was dead in it — which is a
            // real gap in a dialog you reach FROM a mouse-driven table.
            Element r = h(std::move(row)) | gap(0) | hit(hit_signal_row(static_cast<int>(i)));
            if (on) r = std::move(r) | bgc(pal::track);
            body.push_back(r.build());
        }

        body.push_back(blank());
        // Hint row: full labels when the card is roomy, keys-only when tight —
        // pick() renders the first alternative whose measured width fits, so
        // the fixed cells can never fuse ("1-9 /pick enter /send").
        {
            const Style k = Style{}.with_fg(pal::text).with_bold();
            const Style d = Style{}.with_fg(pal::dim);
            Element full = (h(
                text("  ") | nowrap,
                text("1-9 0 a b / ↑↓", k) | nowrap, text(" pick   ", d) | nowrap,
                text("enter / y", k) | nowrap, text(" send   ", d) | nowrap,
                text("esc", k) | nowrap, text(" cancel", d) | nowrap
            ) | gap(0)).build();
            Element compact = (h(
                text("  ") | nowrap,
                text("1-9↑↓", k) | nowrap, text(" pick  ", d) | nowrap,
                text("↵", k) | nowrap, text(" send  ", d) | nowrap,
                text("esc", k) | nowrap, text(" cancel", d) | nowrap
            ) | gap(0)).build();
            body.push_back(Element{pick({full, compact})});
        }

        Element card = Panel("⚑", "SEND SIGNAL", pal::hot)({v(std::move(body))});
        return (v((std::move(card) | width(card_w)))
                | align(Align::Center) | justify(Justify::Center)
                | grow(1) | padding(1)).build();
    }
};

}  // namespace rockbottom::ui
