// widgets/theme_menu.hpp — the theme picker overlay (T).
//
// A centered, searchable list over maya's theme registry. This used to be a
// plain scrolling list, which was fine for rb's old 35-theme deck and is not
// fine for 616: at ~30 visible rows that is 20 screens of arrow-key paging to
// reach "Zenburn". So the picker is a FILTER first and a list second — you
// type, it narrows, and the cursor walks the matches.
//
// ↑↓ (or Ctrl+N/P) move the cursor and LIVE-PREVIEW that theme — the whole UI
// behind the card repaints in it. Enter commits, Esc reverts to whatever was
// active when the menu opened. Each row carries a swatch (the theme's canvas
// plus four accent dots) painted in THAT theme's own colors, so you can see a
// palette before landing on it.

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"
#include "panel.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace rockbottom::ui {

class ThemeMenu {
    int width_  = 100;
    int height_ = 40;
    int sel_    = 0;   // cursor INTO hits_ (also the live-previewed theme)
    std::string query_;
    const std::vector<std::size_t>* hits_ = nullptr;   // deck indices

public:
    ThemeMenu(int w, int h, int sel, std::string query,
              const std::vector<std::size_t>& hits)
        : width_(w), height_(h), sel_(sel), query_(std::move(query)), hits_(&hits) {}

    operator maya::Element() const { return build(); }

    // How many theme rows the card can show at this terminal height. The card
    // spends 2 rows on the border, 1 header, 1 search box, 1 blank, 1 blank,
    // 1 hint = 7 of chrome; the rest are list rows. Clamp so a short terminal
    // still works.
    [[nodiscard]] static int visible_rows(int height) {
        return std::clamp(height - 7 - 4 /*overlay padding slack*/, 4, 40);
    }

    // Window top so `sel` stays visible with a little context. Mirrors the
    // proc-table sticky-scroll idiom but stateless (recomputed each frame).
    [[nodiscard]] static int window_top(int sel, int height, int n) {
        const int vis = visible_rows(height);
        if (n <= vis) return 0;
        int top = sel - vis / 2;              // center the cursor
        return std::clamp(top, 0, n - vis);
    }

private:
    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        const std::vector<std::size_t>& hits = *hits_;
        const int n    = static_cast<int>(hits.size());
        const int vis  = visible_rows(height_);
        const int sel  = n > 0 ? std::clamp(sel_, 0, n - 1) : 0;
        const int top  = window_top(sel, height_, n);
        // Wider than the old card: maya's names are real names ("Catppuccin
        // Mocha", "Everforest Dark Hard"), not the 6-letter slugs rb used.
        const int card_w = std::clamp(width_ - 6, std::min(52, std::max(34, width_ - 4)), 58);

        std::vector<Element> body;

        // Header: the count, and how much of the deck the filter kept.
        body.push_back((h(
            text("pick a theme  ") | nowrap | fgc(pal::dim),
            text(n > 0 ? std::to_string(sel + 1) + "/" + std::to_string(n)
                       : std::string("0/0"))
                | nowrap | Bold | fgc(pal::label),
            text(query_.empty() ? "" : "  of " + std::to_string(theme_count()))
                | nowrap | fgc(pal::faint)
        )).build());

        // The search box. Always visible — a filter you cannot see is a filter
        // you forget is on, and then the list looks broken.
        {
            const bool none = n == 0;
            std::vector<Element> box;
            box.push_back((text("  ") | nowrap).build());
            box.push_back((text("/") | nowrap | Bold
                           | fgc(none ? pal::crit : pal::proc_ac)).build());
            box.push_back((text(query_.empty() ? std::string("type to search")
                                               : query_)
                           | nowrap | fgc(query_.empty() ? pal::faint
                                                         : (none ? pal::crit : pal::white))).build());
            // Block cursor, so an empty query still reads as a live input.
            box.push_back((text("\xe2\x96\x8f") | nowrap | fgc(pal::proc_ac)).build());
            body.push_back((h(std::move(box)) | gap(0)).build());
        }
        body.push_back(blank());

        if (n == 0) {
            // Say so, and say what's still live — the preview didn't change,
            // and a blank card would imply it had.
            body.push_back((text("  no theme matches that") | nowrap | fgc(pal::crit)).build());
            body.push_back(blank());
            body.push_back((h(
                text("  still on ") | nowrap | fgc(pal::dim),
                text(active_theme_name()) | nowrap | Bold | fgc(pal::text)
            ) | gap(0)).build());
        }

        const bool more_above = top > 0;
        const bool more_below = top + vis < n;

        for (int r = 0; r < vis && n > 0; ++r) {
            const int i = top + r;
            if (i >= n) break;
            const std::size_t di = hits[static_cast<std::size_t>(i)];
            const Theme& th = theme_at(di);
            const bool on = i == sel;
            const Color ink = on ? pal::white : pal::text;

            std::vector<Element> row;
            // Selection bar (proc/help-pane idiom).
            row.push_back((text(on ? "\xe2\x96\x8e" : " ") | nowrap | fgc(pal::proc_ac)).build());
            // Deck index — stable across filters, so the number next to a
            // theme means the same thing however you searched for it.
            row.push_back((text(std::to_string(di + 1)) | nowrap
                           | fgc(on ? pal::proc_ac : pal::faint)
                           | width(4) | justify(Justify::End)).build());
            row.push_back((text("  ") | nowrap).build());
            // Name. truncate_end is maya's width-aware clip (the names run to
            // ~24 cols: "Everforest Dark Hard", "Apple System Colors Light").
            row.push_back((text(std::string(maya::truncate_end(th.name, 26))) | nowrap
                           | Bold | fgc(ink) | width(26)).build());
            // Swatch, in THIS theme's own colors (theme_at, not pal::) so the
            // row previews the palette rather than the active one.
            row.push_back((text("  ") | nowrap).build());
            row.push_back((text("\xe2\x96\x88\xe2\x96\x88") | nowrap | fgc(th.bg_panel)).build());
            row.push_back((text("\xe2\x97\x8f") | nowrap | fgc(th.cpu_ac)).build());
            row.push_back((text("\xe2\x97\x8f") | nowrap | fgc(th.mem_ac)).build());
            row.push_back((text("\xe2\x97\x8f") | nowrap | fgc(th.net_ac)).build());
            row.push_back((text("\xe2\x97\x8f") | nowrap | fgc(th.proc_ac)).build());
            // The load ramp, which is the part rb derives rather than reads.
            // Seeing good→warn→hot→crit here is how you tell whether a scheme
            // will actually show you a busy machine.
            row.push_back((text(" ") | nowrap).build());
            row.push_back((text("\xe2\x96\x8c") | nowrap | fgc(th.good)).build());
            row.push_back((text("\xe2\x96\x8c") | nowrap | fgc(th.warn)).build());
            row.push_back((text("\xe2\x96\x8c") | nowrap | fgc(th.hot)).build());
            row.push_back((text("\xe2\x96\x8c") | nowrap | fgc(th.crit)).build());

            Element rowe = h(std::move(row)) | gap(0);
            if (on) rowe = std::move(rowe) | bgc(pal::track);
            body.push_back(rowe.build());
        }

        // Scroll affordance: a dim "· N more ·" line when the list overflows.
        if (more_above || more_below) {
            std::string s;
            if (more_above && more_below)
                s = "  \xe2\x86\x91 " + std::to_string(top) + " more    \xe2\x86\x93 " +
                    std::to_string(n - top - vis) + " more";
            else if (more_above)
                s = "  \xe2\x86\x91 " + std::to_string(top) + " more";
            else
                s = "  \xe2\x86\x93 " + std::to_string(n - top - vis) + " more";
            body.push_back((text(s) | nowrap | fgc(pal::faint)).build());
        }

        body.push_back(blank());
        {
            const Style k = Style{}.with_fg(pal::text).with_bold();
            const Style d = Style{}.with_fg(pal::dim);
            body.push_back((h(
                text("  ") | nowrap,
                text("type", k) | nowrap, text(" filter   ", d) | nowrap,
                text("\xe2\x86\x91\xe2\x86\x93", k) | nowrap, text(" preview   ", d) | nowrap,
                text("enter", k) | nowrap, text(" keep   ", d) | nowrap,
                text("esc", k) | nowrap, text(" revert", d) | nowrap
            ) | gap(0)).build());
        }

        Element card = Panel("\xe2\x97\x91", "THEME", pal::proc_ac)({v(std::move(body))});
        return (v((std::move(card) | width(card_w)))
                | align(Align::Center) | justify(Justify::Center)
                | grow(1) | padding(1)).build();
    }
};

}  // namespace rockbottom::ui
