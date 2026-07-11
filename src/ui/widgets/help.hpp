// widgets/help.hpp — centered help overlay: responsive (card width tracks the
// terminal, columns collapse when narrow) and scrollable (row-windowed body
// with the house scrollbar when the terminal is too short for everything).

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"
#include "detail/common.hpp"
#include "panel.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace rockbottom::ui {

class HelpOverlay {
    int width_  = 100;
    int height_ = 40;
    int scroll_ = 0;

public:
    HelpOverlay(int w, int h, int scroll) : width_(w), height_(h), scroll_(scroll) {}

    operator maya::Element() const { return build(); }

    // One binding: keys column + description. Grouped under section rows.
    struct Entry { const char* keys; const char* desc; };
    struct Group { const char* name; std::vector<Entry> entries; };

    [[nodiscard]] static const std::vector<Group>& groups() {
        static const std::vector<Group> g = {
            {"PROCESSES", {
                {"↑↓ j k", "select process"},
                {"/", "filter by name or pid"},
                {"t", "toggle process tree"},
                {"← →", "collapse / expand subtree (tree)"},
                {"= / +", "collapse-all / expand-all (tree)"},
                {"*", "follow: lock cursor to this process"},
                {"x / Del", "end process (SIGTERM)"},
                {"K", "force-kill (SIGKILL)"},
                {"l", "send any signal (picker)"},
                {"r", "renice (change priority)"},
                {"X", "end ALL with this name"},
                {"y / n", "confirm / cancel kill"},
            }},
            {"SORTING", {
                {"s", "cycle sort column"},
                {"c m i n P o", "cpu · mem · i/o · name · pid · port"},
                {"R / re-press", "reverse sort direction"},
            }},
            {"DETAIL", {
                {"1 2 3 4 5 6", "cpu · mem · net · gpu · disk · proc"},
                {"Enter", "open selected process detail"},
                {"↑↓ / PgUp PgDn", "scroll the detail pane"},
                {"g / G", "jump to top / bottom of pane"},
                {"Esc", "close detail / help"},
            }},
            {"GENERAL", {
                {"p / Space", "pause / resume"},
                {"? / h", "toggle this help"},
                {"q / Esc", "quit"},
            }},
            {"MOUSE", {
                {"click row", "select · header sorts · footer acts"},
                {"right-click", "end process (SIGTERM)"},
                {"wheel", "scroll list · panes · this help"},
            }},
        };
        return g;
    }

    // Total body rows at a given card width — the app uses this to clamp the
    // scroll offset. Must mirror rows() exactly.
    [[nodiscard]] static int content_rows(int term_w) {
        int n = 2;   // tagline + blank
        for (const auto& g : groups())
            n += 1 + static_cast<int>(g.entries.size()) + 1;  // header + rows + gap
        n += 3;      // legend footnotes
        (void)term_w;
        return n;
    }

    // Viewport rows available for the body at a given terminal height:
    // outer padding(2×1) + panel border(2) + panel padding(2) = 6 chrome rows.
    [[nodiscard]] static int viewport_rows(int term_h) {
        return std::max(3, term_h - 8);
    }

private:
    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        // Card width tracks the terminal: ideal 70, but never wider than
        // what fits with the centering margins; floor keeps it legible.
        const int card_w = std::clamp(width_ - 6, 34, 70);
        const int keys_w = card_w >= 56 ? 16 : 12;

        std::vector<Element> body;
        body.push_back((text("A calmer system monitor — it tells you what's happening.")
                        | nowrap | fgc(pal::label)).build());
        body.push_back(blank());

        for (const auto& g : groups()) {
            // Section header: accent tick + spaced-out name, house style.
            body.push_back((h(
                text("▍", Style{}.with_fg(pal::sky)) | nowrap,
                text(g.name, Style{}.with_fg(pal::sky).with_bold()) | nowrap
            )).build());
            for (const auto& e : g.entries) {
                body.push_back((h(
                    text("  ") | nowrap,
                    text(e.keys, Style{}.with_fg(pal::text).with_bold())
                        | nowrap | width(keys_w),
                    text(e.desc, Style{}.with_fg(pal::label)) | nowrap
                ) | gap(1)).build());
            }
            body.push_back(blank());
        }

        body.push_back((text("Banner: green calm · blue busy · orange stressed · red critical.")
                        | nowrap | fgc(pal::dim)).build());
        body.push_back((text("stall chips = kernel PSI: % of time tasks waited on a resource.")
                        | nowrap | fgc(pal::dim)).build());
        body.push_back((text("» marks the culprit · ▎ is your selection.")
                        | nowrap | fgc(pal::dim)).build());

        // Window the body through the shared scroller: slim bar + chevrons
        // appear only when the terminal is too short for the full list.
        const int view_h = viewport_rows(height_);
        Element windowed = detail::scroller(std::move(body), scroll_, view_h, pal::sky);

        Element card = Panel("?", "HELP", pal::sky)({std::move(windowed)});
        return (v((std::move(card) | width(card_w)))
                | align(Align::Center) | justify(Justify::Center)
                | grow(1) | padding(1)).build();
    }
};

}  // namespace rockbottom::ui
