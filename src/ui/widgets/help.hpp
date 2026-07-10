// widgets/help.hpp — centered help overlay built on maya::KeyHelp
// (grouped bindings, width-adaptive columns) wrapped in the house Panel.

#pragma once

#include <maya/maya.hpp>
#include <maya/widget/key_help.hpp>

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

        KeyHelp keys;
        keys.set_title("");
        keys.add("Up/Dn j k", "select process", "Processes");
        keys.add("/", "filter by name or pid", "Processes");
        keys.add("x / Del", "end process (SIGTERM)", "Processes");
        keys.add("K", "force-kill (SIGKILL)", "Processes");
        keys.add("y / n", "confirm / cancel kill", "Processes");
        keys.add("s", "cycle sort column", "Sorting");
        keys.add("c m i n P o", "cpu · mem · i/o · name · pid · port", "Sorting");
        keys.add("1 2 3 4 5 6", "detail: cpu · mem · net · gpu · disk · proc", "Detail");
        keys.add("Enter", "open selected process detail", "Detail");
        keys.add("↑↓ / PgUp PgDn", "scroll the detail pane", "Detail");
        keys.add("g / G", "jump to top / bottom of pane", "Detail");
        keys.add("Esc", "close detail / help", "Detail");
        keys.add("p / Space", "pause / resume", "General");
        keys.add("? / h", "toggle this help", "General");
        keys.add("q / Esc", "quit", "General");
        keys.add("click row", "select · header sorts · footer acts", "Mouse");
        keys.add("right-click", "end process (SIGTERM)", "Mouse");
        keys.add("wheel", "scroll the process list", "Mouse");

        std::vector<Element> body = {
            (text("A calmer system monitor — it tells you what's happening.")
                | fgc(pal::label)).build(),
            blank(),
            keys.build(),
            blank(),
            (text("Banner: green calm · blue busy · orange stressed · red critical.")
                | fgc(pal::dim)).build(),
            (text("stall chips = kernel PSI: % of time tasks waited on a resource.")
                | fgc(pal::dim)).build(),
            (text("» marks the process driving the verdict · ▶ is your selection.")
                | fgc(pal::dim)).build(),
        };

        Element card = Panel("?", "HELP", pal::sky)(std::move(body));
        return (v((std::move(card) | width(70)))
                | align(Align::Center) | justify(Justify::Center)
                | grow(1) | padding(2)).build();
    }
};

}  // namespace bottom::ui
