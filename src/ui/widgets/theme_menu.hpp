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
// ── LAYOUT: every width here is DERIVED, never guessed ────────────────────
// The first version hardcoded `name_w = width_ - 26`, which was wrong by 9
// columns at every size: names truncated to "Apple System Co." while a
// visibly empty gutter sat to their right. The fix is kRowChrome below — the
// row's fixed cells counted once, in one place, so the name gets exactly the
// remainder. If you add or remove a cell in row(), change that constant and
// nothing else needs touching.
//
// Same rule vertically: kChromeRows is the non-list rows counted honestly.
// It was under by one, so the second hint line was silently clipped off the
// bottom on every terminal.

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
    bool docked_ = true;        // side panel, or centered card fallback

    // Panel::operator() draws a rounded border (1 cell each side) and pads
    // the interior by 1 more. Everything inside therefore gets width_ - 4.
    static constexpr int kPanelChrome = 4;

    // Fixed cells in a list row, left to right:
    //   1 selection bar
    //   1 light/dark glyph
    //   1 gap
    //   … name (everything left over)
    //   1 gap
    //   1 canvas block
    //   2 domain accent dots
    //   1 gap
    //   4 load-ramp bars
    //   1 current-theme dot
    static constexpr int kRowChrome = 13;

    // Non-list rows, counted against a real render rather than guessed —
    // getting this wrong is invisible in code review and obvious on screen.
    // It was 9 (second hint line clipped off the bottom), then 10 (two dead
    // rows above the border). Measured:
    //   1 search  1 chips  1 blank  1 position bar  1 hint = 5
    //   + 2 panel border rows = 7
    // The panel's interior padding is horizontal only, so it costs no rows.
    static constexpr int kChromeRows = 7;

public:
    ThemeMenu(int w, int h, int sel, std::string query,
              const std::vector<std::size_t>& hits,
              ThemeMode mode, int hover, int top, int recent, std::size_t restore,
              bool docked)
        : width_(w), height_(h), sel_(sel), query_(std::move(query)), hits_(&hits),
          mode_(mode), hover_(hover), top_(top), recent_(recent), restore_(restore),
          docked_(docked) {}

    operator maya::Element() const { return build(); }

    // Below this the dashboard has nothing left worth previewing, so the
    // picker goes back to being a full-screen card.
    static constexpr int kMinDockWidth = 104;

    // Width to dock at, or 0 for "don't dock, show the card".
    //
    // Responsive rather than a fixed 42: on a 110-col terminal a 42-wide
    // panel eats 38% of the screen and squeezes the process table into
    // uselessness, while on a 240-col ultrawide the same 42 leaves theme
    // names truncated with a third of the screen empty beside them. Scale
    // with the terminal and clamp to a band that is always legible (the
    // longest names — "Apple System Colors Light" — want ~25 columns) and
    // never greedy.
    [[nodiscard]] static int panel_width(int term_w) {
        if (term_w < kMinDockWidth) return 0;
        const int want = term_w * 30 / 100;
        return std::clamp(want, 34, 54);
    }

    // How many theme rows fit.
    //
    // The card fallback wraps the panel in padding(1), which costs 2 more
    // rows than the docked path — unaccounted for, the list overran and
    // pushed the hint footer off the bottom on every narrow terminal.
    [[nodiscard]] static int visible_rows(int height, bool docked = true) {
        return std::clamp(height - kChromeRows - (docked ? 0 : 2), 3, 80);
    }

private:
    // Width available to the name cell.
    //
    // Capped, not just floored: on a 200-col terminal the panel is 54 wide
    // and an uncapped name column strands the swatch 20 cells away from the
    // name it describes, so the eye has to travel to pair them up. 30 fits
    // every name in the registry ("Apple System Colors Light" is the longest
    // at 25), and the leftover goes to a gap that pushes the swatch to the
    // right edge where it forms a clean column.
    [[nodiscard]] int name_width() const {
        const int avail = width_ - kPanelChrome - kRowChrome;
        return std::clamp(avail, 10, 30);
    }

    // One row of the list.
    [[nodiscard]] maya::Element row(int i) const {
        using namespace maya;
        using namespace maya::dsl;

        const std::size_t di = (*hits_)[static_cast<std::size_t>(i)];
        const Theme& th = theme_at(di);
        const bool on     = i == sel_;
        const bool hov    = i == hover_;
        const bool is_cur = di == restore_;
        const bool light  = theme_is_light(di);

        std::vector<Element> cells;

        // Selection bar. A hovered-but-not-selected row gets a dimmer mark so
        // the pointer has feedback without pretending to be the cursor.
        cells.push_back((text(on ? "\xe2\x96\x8e" : (hov ? "\xe2\x94\x82" : " "))
                         | nowrap | fgc(on ? pal::proc_ac : pal::dim)).build());

        // Light/dark glyph. Half-filled circles rather than the words, which
        // would cost 5 columns the names need.
        cells.push_back((text(light ? "\xe2\x97\x90" : "\xe2\x97\x91") | nowrap
                         | fgc(light ? pal::amber : pal::dim)).build());
        cells.push_back((text(" ") | nowrap).build());

        // Name, with the matched characters lit. This is what makes a fuzzy
        // match explicable: without it, "cmocha" matching "Catppuccin Mocha"
        // looks arbitrary, and you can't tell why one row outranks another.
        {
            const int nw = name_width();
            const std::string shown{maya::truncate_end(th.name, static_cast<std::size_t>(nw))};
            const std::vector<std::size_t> hl = theme_match_positions(di, query_);
            const Color ink = on ? pal::white : (is_cur ? pal::text : pal::label);

            std::vector<StyledRun> runs;
            if (!hl.empty()) {
                const Style base = Style{}.with_fg(ink).with_bold();
                const Style lit  = Style{}.with_fg(pal::proc_ac).with_bold();
                std::size_t k = 0;
                for (std::size_t c = 0; c < shown.size(); ++c) {
                    while (k < hl.size() && hl[k] < c) ++k;
                    runs.push_back({c, 1, (k < hl.size() && hl[k] == c) ? lit : base});
                }
            } else {
                runs.push_back({0, shown.size(), Style{}.with_fg(ink).with_bold()});
            }
            cells.push_back((Element{TextElement{.content = shown, .style = {},
                                                 .wrap = TextWrap::NoWrap,
                                                 .runs = std::move(runs)}}
                             | width(nw)).build());
        }

        // Swatch in THIS theme's colours (theme_at, not pal::) so the row
        // previews the palette rather than the active one: canvas, two domain
        // accents, then the load ramp — the part rb derives, and the part
        // that decides whether a busy machine will read.
        //
        // grow(1) on the gap right-aligns the whole swatch, so on a wide
        // panel the blocks form a straight column at the edge instead of
        // ragging along behind names of different lengths.
        cells.push_back(((text(" ") | nowrap) | grow(1)).build());
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
        cells.push_back((text(is_cur ? "\xc2\xb7" : " ") | nowrap
                         | fgc(pal::proc_ac)).build());

        Element r = h(std::move(cells)) | gap(0) | hit(hit_theme_row(i));
        if (on)       r = std::move(r) | bgc(pal::sel_bg);
        else if (hov) r = std::move(r) | bgc(pal::track);
        return r.build();
    }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        const std::vector<std::size_t>& hits = *hits_;
        const int n     = static_cast<int>(hits.size());
        const int vis   = visible_rows(height_, docked_);
        const int inner = std::max(8, width_ - kPanelChrome);
        const int sel   = n > 0 ? std::clamp(sel_, 0, n - 1) : 0;
        const int top   = n <= vis ? 0 : std::clamp(top_, 0, n - vis);

        std::vector<Element> body;

        // ── search box ──
        // Always visible, because a filter you can't see is a filter you
        // forget is on — and then the list just looks broken.
        {
            const bool none = n == 0;
            const std::string count = n > 0 ? std::to_string(sel + 1) + "/" + std::to_string(n)
                                            : std::string("0");
            body.push_back((h(
                text("/") | nowrap | fgc(none ? pal::crit : pal::proc_ac),
                text(query_.empty() ? std::string("search") : query_) | nowrap
                    | fgc(query_.empty() ? pal::faint : (none ? pal::crit : pal::white)),
                text("\xe2\x96\x8f") | nowrap | fgc(pal::proc_ac),
                (text(count) | nowrap | fgc(none ? pal::crit : pal::dim))
                    | grow(1) | justify(Justify::End)
            ) | gap(0)).build());
        }

        // ── mode chips ──
        // Clickable; the keyboard route (Ctrl+D / Ctrl+L) is on the hint
        // line. Dark/light is the single most consequential axis and halves
        // the deck instantly.
        {
            auto chip = [&](const char* label, ThemeMode mm) {
                const bool act = mode_ == mm;
                return (text(std::string(" ") + label + " ") | nowrap
                        | fgc(act ? pal::bg : pal::dim)
                        | bgc(act ? pal::proc_ac : pal::track)
                        | hit(hit_theme_mode(static_cast<std::uint32_t>(mm)))).build();
            };
            body.push_back((h(
                chip("all", ThemeMode::All),
                chip("dark", ThemeMode::Dark),
                chip("light", ThemeMode::Light)
            ) | gap(1)).build());
        }
        body.push_back(blank());

        if (n == 0) {
            // Say so, and say what is still live — the preview did not
            // change, and a blank panel would imply that it had.
            body.push_back((text("no match") | nowrap | Bold | fgc(pal::crit)).build());
            body.push_back(blank());
            body.push_back((text(std::string("still on ") + active_theme_name())
                            | nowrap | fgc(pal::dim)).build());
        }

        for (int r = 0; r < vis && n > 0; ++r) {
            const int i = top + r;
            if (i >= n) break;
            // Divider under the recently-used block, so the pinned rows read
            // as a group rather than as a strange sort order. Labelled on the
            // RIGHT so the label doesn't collide with the name column, and
            // inset by the selection-bar cell so it lines up with the rows.
            if (recent_ > 0 && i == recent_ && query_.empty() && r > 0) {
                const std::string lbl = "all themes";
                const int rule = std::max(0, inner - static_cast<int>(lbl.size()) - 3);
                body.push_back((h(
                    text(" " + std::string(static_cast<std::size_t>(rule), '-') + " ")
                        | nowrap | fgc(pal::faint),
                    text(lbl) | nowrap | fgc(pal::faint)
                ) | gap(0)).build());
            }
            body.push_back(row(i));
        }

        // ── position ──
        // A real proportional bar: with 616 rows "↓ 580 more" says nothing
        // about where you are, but a filled track does. Only drawn when the
        // list actually overflows — a short list gets a spacer that GROWS
        // instead, which pins the hints to the bottom edge. Without that,
        // filtering to seven rows left the hint lines stranded in the middle
        // of the panel with a field of dead space under them.
        if (n > vis) {
            const std::string pos = std::to_string(top + 1) + "-" +
                                    std::to_string(std::min(n, top + vis));
            const int bar_w = std::max(6, inner - static_cast<int>(pos.size()) - 2);
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
                (text(pos) | nowrap | fgc(pal::faint)) | grow(1) | justify(Justify::End)
            ) | gap(1)).build());
        } else {
            body.push_back((v() | grow(1)).build());
        }

        // ── hint footer: exactly ONE row, always ──
        //
        // It was two rows, which is one too many for a strip whose job is to
        // be glanceable: it cost a list row at every size, and on a short
        // terminal that is a real fraction of what you can see. One row also
        // gives the panel a clean single-line base instead of a two-line
        // block that reads as a second widget.
        //
        // The full legend is 55 cells and the inner width runs 30..50 (the
        // panel is clamped 34..54, less 4 for border + padding), so it never
        // fits whole and cannot simply be truncated — clipping would silently
        // eat the dark/light and jump-to-current keys, which are the two
        // least discoverable things here. Instead it degrades in planned
        // steps measured against those real widths, dropping the WORDS before
        // the KEYS: a bare `^d/^l` still says the binding exists and is worth
        // trying, where a cut-off "^d/^l dark/li" just says the panel is
        // broken.
        {
            const Style k = Style{}.with_fg(pal::text).with_bold();
            const Style d = Style{}.with_fg(pal::dim);
            std::vector<Element> hint;
            auto key = [&](const char* s) { hint.push_back((text(s, k) | nowrap).build()); };
            auto lbl = [&](const char* s) { hint.push_back((text(s, d) | nowrap).build()); };

            if (inner >= 50) {          // widest panel (54): everything spelled out
                key("\xe2\x86\x91\xe2\x86\x93"); lbl(" move  ");
                key("\xe2\x8f\x8e");             lbl(" keep  ");
                key("esc");                      lbl(" back  ");
                key("^d/^l");                    lbl(" d/l  ");
                key("^g");                       lbl(" cur");
            } else if (inner >= 41) {   // mid (45): drop the modifier labels
                key("\xe2\x86\x91\xe2\x86\x93"); lbl(" move  ");
                key("\xe2\x8f\x8e");             lbl(" keep  ");
                key("esc");                      lbl(" back  ");
                key("^d/^l");                    lbl("  ");
                key("^g");
            } else if (inner >= 29) {   // narrowest panel (34): inner is 30,
                                        // so this tier must fit in 29
                key("\xe2\x86\x91\xe2\x86\x93"); lbl(" move  ");
                key("\xe2\x8f\x8e");             lbl(" keep  ");
                key("esc");                      lbl(" ");
                key("^d/^l");                    lbl(" ");
                key("^g");
            } else {                    // card fallback at a tiny width
                // Keys only. Still complete — nothing is hidden, it has just
                // stopped explaining itself.
                key("\xe2\x86\x91\xe2\x86\x93"); lbl(" ");
                key("\xe2\x8f\x8e");             lbl(" ");
                key("esc");                      lbl(" ");
                key("^d/^l");                    lbl(" ");
                key("^g");
            }
            body.push_back((h(std::move(hint)) | gap(0)).build());
        }

        // The chip counts the deck so the header carries the scale without
        // spending a body row on it.
        //
        // .grow(1) matters when docked: without it the Panel sizes to its
        // content and the border closes early, leaving a band of unpainted
        // canvas below a short list. The card path keeps its natural height.
        Panel p("\xe2\x97\x91", "THEME", pal::proc_ac);
        p.chip(std::to_string(theme_count()));
        if (docked_) p.grow(1);
        Element card = p({v(std::move(body)) | grow(1)});

        // Docked: fill the column edge to edge. Undocked: a centered card,
        // for terminals too narrow to show both.
        if (docked_)
            return (v(std::move(card) | grow(1)) | width(width_) | height(height_)).build();
        return (v((std::move(card) | width(std::clamp(width_ - 6, 34, 58))))
                | align(Align::Center) | justify(Justify::Center)
                | grow(1) | padding(1)).build();
    }
};

}  // namespace rockbottom::ui
