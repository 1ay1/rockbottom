// widgets/help.hpp — centered help overlay card.

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"
#include "panel.hpp"

#include <vector>

namespace bottom::ui {

class HelpOverlay {
public:
    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        auto row = [](const char* k, const char* d) -> Element {
            return (h(
                text(k) | nowrap | Bold | fgc(pal::sky) | w_<12>,
                text(d) | nowrap | fgc(pal::text)
            )).build();
        };

        std::vector<Element> body = {
            (text("A calmer system monitor — it tells you what's happening.")
                | fgc(pal::label)).build(),
            blank(),
            row("q / Esc",   "quit"),
            row("p / Space", "pause / resume sampling"),
            row("s",         "cycle sort column"),
            row("c",         "sort by CPU"),
            row("m",         "sort by memory"),
            row("n",         "sort by name"),
            row("P",         "sort by PID"),
            row("? / h",     "toggle this help"),
            blank(),
            (text("Banner: green calm · blue busy · orange stressed · red critical.")
                | fgc(pal::dim)).build(),
            (text("The » row is the process driving the current verdict.")
                | fgc(pal::dim)).build(),
        };

        Element card = Panel("?", "HELP", pal::sky)(std::move(body));
        return (v((std::move(card) | width(66)))
                | align(Align::Center) | justify(Justify::Center)
                | grow(1) | padding(2)).build();
    }
};

}  // namespace bottom::ui
