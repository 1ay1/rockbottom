// widgets/theme_menu.hpp — the theme picker (T).
//
// A DOCKED, searchable panel over maya's theme registry — not a modal.
//
// It used to be a centered card, which was the wrong shape for what this
// does. The picker's entire value is the live preview, and a centered card
// covers the thing being previewed: you moved the cursor, watched a card
// change colour, committed, and only then discovered what the process table
// looked like. Docked to the right, the dashboard renders beside it at a
// reduced width and repaints in each theme as you move, so you are choosing
// by looking at the actual UI rather than at a swatch.
//
// It is also a FILTER first and a list second. 616 themes at ~30 rows is 20
// screens of arrow-key paging to reach "Zenburn", so typing narrows and the
// cursor walks the matches. Matched characters are highlighted, each row
// carries the theme's own canvas + accents + load ramp painted in THAT
// theme's colours, and a ◐/◑ glyph marks light vs dark — the one property
// you cannot infer from a name ("Ayu" is dark, "Ayu Light" is not).
//
// On a terminal too narrow to host both (< kMinDockWidth), panel_width()
// returns 0 and the caller falls back to a full-width card: a squeezed
// two-column layout on an 80-col terminal would wreck the preview this
// exists to show.

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"
#include "hit_ids.hpp"
#include "panel.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace rockbottom::ui {

class ThemeMenu {
    int width_  = 44;
    int height_ = 40;
    int sel_    = 0;   // cursor INTO hits_ (also the live-previewed theme)
    std::string query_;
    const std::vector<std::size_t>* hits_ = nullptr;   // deck indices
    ThemeMode mode_ = ThemeMode::All;
    int hover_  = -1;
    int top_    = 0;   // first visible row (model-owned, so the wheel can
                       // move the window without moving the preview)
    int recent_ = 0;   // leading rows that are recently-used
    std::size_t restore_ = 0;   // the theme we'd revert to on Esc

public:
    ThemeMenu(int w, int h, int sel, std::string query,
              const std::vector<std::size_t>& hits,
              ThemeMode mode, int hover, int top, int recent, std::size_t restore)
        : width_(w), height_(h), sel_(sel), query_(std::move(query)), hits_(&hits),
          mode_(mode), hover_(hover), top_(top), recent_(recent), restore_(restore) {}

    operator maya::Element() const { return build(); }

    // Below this the dashboard has no room left to be worth previewing, so
    // the picker goes back to being a full-screen card.
    static constexpr int kMinDockWidth = 108;
    static constexpr int kPanelWidth   = 42;

    // Width to dock at, or 0 for "don't dock, show the card".
    [[nodiscard]] static int panel_width(int term_w) {
        if (term_w < kMinDockWidth) return 0;
        // Never take more than a third: the dashboard is the subject here,
        // the picker is the instrument.
        return std::min(kPanelWidth, term_w / 3);
    }

    // How many theme rows fit. The panel spends 2 rows on the border, 1 on
    // the header, 1 on the search box, 1 on the mode chips, 1 blank, 2 on the
    // hint = 8 of chrome. App::theme_rows() calls THIS, so the key handler's
    // page-size and the paint can't drift apart.
    [[nodiscard]] static int visible_rows(int height) {
        return std::clamp(height - 9, 4, 60);
    }

private:
    // One row of the list.
    [[nodiscard]] maya::Element row(int i, bool docked) const {
        using namespace maya;
        using namespace maya::dsl;

        const std::size_t di = (*hits_)[static_cast<std::size_t>(i)];
        const Theme& th = theme_at(di);
        const bool on    = i == sel_;
        const bool hov   = i == hover_;
        const bool is_cur = di == restore_;

        std::vector<Element> cells;

        // Selection bar. A hovered-but-not-selected row gets a dimmer mark so
        // the pointer has feedback without pretending to be the cursor.
        cells.push_back((text(on ? "\xe2\x96\x8e" : (hov ? "\xe2\x94\x82" : " "))
                         | nowrap | fgc(on ? pal::proc_ac : pal::dim)).build());

        // Light/dark glyph. Half-filled circles rather than the words, which
        // would cost 5 columns the names need.
        cells.push_back((text(theme_is_light(di) ? "\xe2\x97\x90" : "\xe2\x97\x91")
                         | nowrap
                         | fgc(theme_is_light(di) ? pal::amber : pal::dim)).build());
        cells.push_back((text(" ") | nowrap).build());

        // Name, with the matched characters lit. This is what makes a fuzzy
        // match explicable: without it, "cmocha" matching "Catppuccin Mocha"
        // looks arbitrary, and you can't tell why one row outranks another.
        {
            const std::string nm = th.name;
            const int name_w = docked ? std::max(12, width_ - 26) : 26;
            const std::string shown{maya::truncate_end(nm, static_cast<std::size_t>(name_w))};
            const std::vector<std::size_t> hl = theme_match_positions(di, query_);
            const Color ink = on ? pal::white : (is_cur ? pal::text : pal::label);

            std::vector<StyledRun> runs;
            if (!hl.empty()) {
                const Style base  = Style{}.with_fg(ink).with_bold();
                const Style lit   = Style{}.with_fg(pal::proc_ac).with_bold();
                std::size_t k = 0;
                for (std::size_t c = 0; c < shown.size(); ++c) {
                    while (k < hl.size() && hl[k] < c) ++k;
                    const bool is_hit = k < hl.size() && hl[k] == c;
                    runs.push_back({c, 1, is_hit ? lit : base});
                }
            } else {
                runs.push_back({0, shown.size(), Style{}.with_fg(ink).with_bold()});
            }
            cells.push_back((Element{TextElement{.content = shown, .style = {},
                                                 .wrap = TextWrap::NoWrap,
                                                 .runs = std::move(runs)}}
                             | width(name_w)).build());
        }

        // Swatch in THIS theme's colours (theme_at, not pal::) so the row
        // previews the palette rather than the active one: canvas, two
        // domain accents, then the load ramp — which is the part rb derives
        // and the part that decides whether a busy machine will read.
        cells.push_back((text(" ") | nowrap).build());
        cells.push_back((text("\xe2\x96\x88") | nowrap | fgc(th.bg_panel)).build());
        cells.push_back((text("\xe2\x97\x8f") | nowrap | fgc(th.cpu_ac)).build());
        cells.push_back((text("\xe2\x97\x8f") | nowrap | fgc(th.mem_ac)).build());
        cells.push_back((text(" ") | nowrap).build());
        cells.push_back((text("\xe2\x96\x8c") | nowrap | fgc(th.good)).build());
        cells.push_back((text("\xe2\x96\x8c") | nowrap | fgc(th.warn)).build());
        cells.push_back((text("\xe2\x96\x8c") | nowrap | fgc(th.hot)).build());
        cells.push_back((text("\xe2\x96\x8c") | nowrap | fgc(th.crit)).build());
        // A dot marks the theme you arrived with, so "what was I using?"
        // never requires remembering.
        cells.push_back((text(is_cur ? "\xc2\xb7" : " ") | nowrap | fgc(pal::dim)).build());

        Element r = h(std::move(cells)) | gap(0) | hit(hit_theme_row(i));
        if (on)       r = std::move(r) | bgc(pal::sel_bg);
        else if (hov) r = std::move(r) | bgc(pal::track);
        return r.build();
    }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        const std::vector<std::size_t>& hits = *hits_;
        const int n   = static_cast<int>(hits.size());
        const int vis = visible_rows(height_);
        const bool docked = width_ <= kPanelWidth;
        const int sel = n > 0 ? std::clamp(sel_, 0, n - 1) : 0;
        int top = n <= vis ? 0 : std::clamp(top_, 0, n - vis);

        std::vector<Element> body;

        // ── search box ──
        // Always visible, because a filter you can't see is a filter you
        // forget is on — and then the list just looks broken.
        {
            const bool none = n == 0;
            body.push_back((h(
                text("\xef\x80\x82") | nowrap | fgc(pal::dim),
                text(query_.empty() ? std::string("search\xe2\x80\xa6") : query_)
                    | nowrap
                    | fgc(query_.empty() ? pal::faint : (none ? pal::crit : pal::white)),
                text("\xe2\x96\x8f") | nowrap | fgc(pal::proc_ac),
                (text(n > 0 ? std::to_string(sel + 1) + "/" + std::to_string(n)
                            : std::string("0"))
                 | nowrap | fgc(none ? pal::crit : pal::dim)) | grow(1) | justify(Justify::End)
            ) | gap(1)).build());
        }

        // ── mode chips ──
        // Clickable, and the keyboard route (Ctrl+D / Ctrl+L) is on the hint
        // line. Dark/light is the single most consequential axis and halves
        // the deck instantly.
        {
            auto chip = [&](const char* label, ThemeMode mm) {
                const bool sel_m = mode_ == mm;
                return (text(std::string(" ") + label + " ") | nowrap
                        | fgc(sel_m ? pal::bg : pal::dim)
                        | bgc(sel_m ? pal::proc_ac : pal::track)
                        | hit(hit_theme_mode(static_cast<std::uint32_t>(mm)))).build();
            };
            body.push_back((h(
                chip("all",   ThemeMode::All),
                chip("dark",  ThemeMode::Dark),
                chip("light", ThemeMode::Light)
            ) | gap(1)).build());
        }
        body.push_back(blank());

        if (n == 0) {
            // Say so, and say what is still live — the preview did not
            // change, and a blank panel would imply that it had.
            body.push_back((text("no match") | nowrap | fgc(pal::crit)).build());
            body.push_back(blank());
            body.push_back((text(std::string("still on ") + active_theme_name())
                            | nowrap | fgc(pal::dim)).build());
        }

        for (int r = 0; r < vis && n > 0; ++r) {
            const int i = top + r;
            if (i >= n) break;
            // Divider under the recently-used block, so the pinned rows read
            // as a separate group rather than as a strange sort order.
            if (recent_ > 0 && i == recent_ && query_.empty()) {
                body.push_back((text(std::string(static_cast<std::size_t>(
                                        std::max(0, width_ - 4)), '-'))
                                | nowrap | fgc(pal::faint)).build());
            }
            body.push_back(row(i, docked));
        }

        // ── position readout ──
        // A real proportional bar: with 616 rows, "↓ 580 more" tells you
        // nothing useful about where you are, but a filled track does.
        if (n > vis) {
            const int bar_w = std::max(8, width_ - 12);
            const int filled = std::max(1, bar_w * vis / n);
            const int start = (bar_w - filled) * top / std::max(1, n - vis);
            std::string track;
            std::vector<StyledRun> runs;
            for (int i = 0; i < bar_w; ++i) {
                const bool lit = i >= start && i < start + filled;
                const std::size_t off = track.size();
                track += lit ? "\xe2\x94\x81" : "\xe2\x94\x80";
                runs.push_back({off, track.size() - off,
                                Style{}.with_fg(lit ? pal::proc_ac : pal::faint)});
            }
            body.push_back((h(
                Element{TextElement{.content = std::move(track), .style = {},
                                    .wrap = TextWrap::NoWrap, .runs = std::move(runs)}},
                text("  " + std::to_string(top + 1) + "-" +
                     std::to_string(std::min(n, top + vis)))
                    | nowrap | fgc(pal::faint)
            ) | gap(0)).build());
        }

        body.push_back(blank());
        {
            const Style k = Style{}.with_fg(pal::text).with_bold();
            const Style d = Style{}.with_fg(pal::dim);
            body.push_back((h(
                text("type", k) | nowrap, text(" filter  ", d) | nowrap,
                text("\xe2\x86\x91\xe2\x86\x93", k) | nowrap, text(" preview  ", d) | nowrap,
                text("\xe2\x8f\x8e", k) | nowrap, text(" keep", d) | nowrap
            ) | gap(0)).build());
            body.push_back((h(
                text("^d/^l", k) | nowrap, text(" dark/light  ", d) | nowrap,
                text("^g", k) | nowrap, text(" back  ", d) | nowrap,
                text("esc", k) | nowrap, text(" revert", d) | nowrap
            ) | gap(0)).build());
        }

        Element card = Panel("\xe2\x97\x91", "THEME", pal::proc_ac)({v(std::move(body))});

        // Docked: fill the column, flush to the edge. Undocked: the old
        // centered card, for terminals too narrow to show both.
        if (docked)
            return (v(std::move(card) | grow(1)) | width(width_) | height(height_)).build();
        return (v((std::move(card) | width(std::clamp(width_ - 6, 34, 58))))
                | align(Align::Center) | justify(Justify::Center)
                | grow(1) | padding(1)).build();
    }
};

}  // namespace rockbottom::ui
