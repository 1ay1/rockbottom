// widgets/theme_menu.hpp — the theme picker overlay (T).
//
// A centered, scrolling list of every theme in the deck. ↑↓ / j k move the
// cursor and LIVE-PREVIEW that theme (the whole UI behind the card repaints in
// it); Enter commits, Esc reverts to whatever was active when the menu opened.
// Each row carries a tiny swatch — the theme's canvas plus four accent dots —
// so you can see a palette before you land on it.

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"
#include "panel.hpp"

#include <string>
#include <vector>

namespace rockbottom::ui {

class ThemeMenu {
    int width_  = 100;
    int height_ = 40;
    int sel_    = 0;   // highlighted deck index (also the live-previewed theme)

public:
    ThemeMenu(int w, int h, int sel) : width_(w), height_(h), sel_(sel) {}

    operator maya::Element() const { return build(); }

    // How many theme rows the card can show at this terminal height. The card
    // spends 2 rows on the border, 1 header, 1 blank, 1 blank, 1 hint = 6 of
    // chrome; the rest are list rows. Clamp so a short terminal still works.
    [[nodiscard]] static int visible_rows(int height) {
        return std::clamp(height - 6 - 4 /*overlay padding slack*/, 4,
                          static_cast<int>(theme_count()));
    }

    // Window top so `sel` stays visible with a little context. Mirrors the
    // proc-table sticky-scroll idiom but stateless (recomputed each frame).
    [[nodiscard]] static int window_top(int sel, int height) {
        const int n   = static_cast<int>(theme_count());
        const int vis = visible_rows(height);
        if (n <= vis) return 0;
        int top = sel - vis / 2;              // center the cursor
        return std::clamp(top, 0, n - vis);
    }

private:
    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        const int n    = static_cast<int>(theme_count());
        const int vis  = visible_rows(height_);
        const int top  = window_top(sel_, height_);
        const int card_w = std::clamp(width_ - 6, std::min(40, std::max(28, width_ - 4)), 46);

        std::vector<Element> body;
        body.push_back((h(
            text("pick a theme  ") | nowrap | fgc(pal::dim),
            text(std::to_string(sel_ + 1) + "/" + std::to_string(n))
                | nowrap | Bold | fgc(pal::label)
        )).build());
        body.push_back(blank());

        const bool more_above = top > 0;
        const bool more_below = top + vis < n;

        for (int r = 0; r < vis; ++r) {
            const int i = top + r;
            if (i >= n) break;
            const Theme& th = theme_at(static_cast<std::size_t>(i));
            const bool on = i == sel_;
            const Color ink = on ? pal::white : pal::text;

            std::vector<Element> row;
            // Selection bar (proc/help-pane idiom).
            row.push_back((text(on ? "▎" : " ") | nowrap | fgc(pal::proc_ac)).build());
            // Index (or ● for the current live theme when it's not the cursor).
            row.push_back((text(std::to_string(i + 1)) | nowrap
                           | fgc(on ? pal::proc_ac : pal::faint)
                           | width(3) | justify(Justify::End)).build());
            row.push_back((text("  ") | nowrap).build());
            // Name.
            row.push_back((text(th.name) | nowrap | Bold | fgc(ink) | width(14)).build());
            // Swatch: canvas block + four accent dots, painted in THIS theme's
            // own colors (theme_at, not pal::) so the row previews the palette.
            row.push_back((text("  ") | nowrap).build());
            row.push_back((text("██") | nowrap | fgc(th.bg_panel)).build());
            row.push_back((text("●") | nowrap | fgc(th.cpu_ac)).build());
            row.push_back((text("●") | nowrap | fgc(th.mem_ac)).build());
            row.push_back((text("●") | nowrap | fgc(th.net_ac)).build());
            row.push_back((text("●") | nowrap | fgc(th.proc_ac)).build());

            Element rowe = h(std::move(row)) | gap(0);
            if (on) rowe = std::move(rowe) | bgc(pal::track);
            body.push_back(rowe.build());
        }

        // Scroll affordance: a dim "· N more ·" line when the list overflows.
        if (more_above || more_below) {
            std::string s;
            if (more_above && more_below)
                s = "  ↑ " + std::to_string(top) + " more    ↓ " +
                    std::to_string(n - top - vis) + " more";
            else if (more_above)
                s = "  ↑ " + std::to_string(top) + " more";
            else
                s = "  ↓ " + std::to_string(n - top - vis) + " more";
            body.push_back((text(s) | nowrap | fgc(pal::faint)).build());
        }

        body.push_back(blank());
        {
            const Style k = Style{}.with_fg(pal::text).with_bold();
            const Style d = Style{}.with_fg(pal::dim);
            body.push_back((h(
                text("  ") | nowrap,
                text("↑↓ / j k", k) | nowrap, text(" preview   ", d) | nowrap,
                text("enter", k) | nowrap, text(" keep   ", d) | nowrap,
                text("esc", k) | nowrap, text(" revert", d) | nowrap
            ) | gap(0)).build());
        }

        Element card = Panel("◑", "THEME", pal::proc_ac)({v(std::move(body))});
        return (v((std::move(card) | width(card_w)))
                | align(Align::Center) | justify(Justify::Center)
                | grow(1) | padding(1)).build();
    }
};

}  // namespace rockbottom::ui
