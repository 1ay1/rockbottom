// widgets/help.hpp — centered help overlay: responsive (card width tracks the
// terminal, columns collapse when narrow) and scrollable (row-windowed body
// with the house scrollbar when the terminal is too short for everything).

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"
#include "detail/common.hpp"
#include "logo.hpp"
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
                {"g / G", "jump to top / bottom of the list"},
                {"PgUp PgDn", "page up / down by one screenful"},
                {"H / M / L", "cursor to top / middle / bottom of the screen"},
                {"/", "filter: name/pid, or user: state: port: cpu: mem: !neg"},
                {"^w / ^u", "filter: delete last term / clear it"},
                {"t", "toggle FLOW tree ↔ flat list"},
                {"← →", "collapse / expand subtree (flow)"},
                {"= / +", "collapse-all / expand-all (flow)"},
                {"▁▅█ gutter", "CPU share vs siblings — tallest/brightest = the hog"},
                {"*", "pin: hoist this process to the top & keep it there"},
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
                {"1 2 3 4 5 6 7", "cpu · mem · net · gpu · disk · proc · users"},
                {"Enter", "open selected process detail"},
                {"↑↓ / PgUp PgDn", "walk the list / scroll the pane"},
                {"← →", "proc pane: walk to parent / busiest child"},
                {"r", "proc pane: renice this process"},
                {"T", "proc pane: end this process + its whole subtree"},
                {"x K l X", "proc pane: stop · kill · signal · end-all-by-name"},
                {"g / G", "jump to top / bottom of pane"},
                {"Esc", "close detail / help"},
            }},
            {"USERS PANE (7)", {
                {"↑↓ / g G", "move the row cursor (this is what X targets)"},
                {"Enter", "open that user's FULL dashboard — cpu, ram, i/o,"},
                {"", "disk, sessions, ports, their heaviest processes"},
                {"/", "find a user — name, uid, real name, home or shell"},
                {"esc", "back one level: dashboard → filter → close"},
                {"click", "select a row · double-click opens the dashboard"},
                {"click a header", "sorts by it · click again reverses (▼ / ▲)"},
                {"c m u i D n", "sort by cpu · mem · procs · i/o · disk · name"},
                {"", "press the same key again to reverse the order"},
                {"f", "filter the PROCESS list to this user, and leave"},
                {"d", "open the disk pane (lowercase d is never a sort here)"},
                {"●2 badge", "live login sessions — someone is actually on the box"},
                {"·svc", "a system/daemon account, sorted below real people"},
                {"DISK —", "not measured yet (no quota; scan runs in background)"},
                {"DISK ≥N", "a floor — the budgeted scan was truncated"},
                {"DISK a/b", "usage against that user's quota"},
                {"DISK%", "share of the filesystem — or of their quota (N%q)"},
                {"X", "end EVERY process this user owns (SIGTERM)"},
                {"K", "force-kill every process this user owns (SIGKILL)"},
                {"—", "root is refused; your own rb process is never a target"},
            }},
            {"GENERAL", {
                {"p / Space", "pause / resume"},
                {"< / > (or , / .)", "slower / faster refresh (250ms–5s)"},
                {"T", "theme picker \xe2\x80\x94 docked, type to search 616, live"},
                {"? / h", "toggle this help (h = collapse in flow tree)"},
                {"q / Esc", "quit"},
            }},
            {"MOUSE", {
                {"hover row", "highlight the process under the pointer"},
                {"click row", "pin + select · headers sort · every hint is live"},
                {"double-click", "open the process detail pane"},
                {"right-click", "a process: end it \xc2\xb7 anywhere else: go back"},
                {"drag scrollbar", "slide any list / pane to any position"},
                {"wheel", "scroll list · panes · this help"},
            }},
        };
        return g;
    }

    // Rows the body actually occupies at this width.
    //
    // MEASURED, not estimated. This used to add up a hand-written tally:
    //   8 /*logo+blank*/ + body + 1 /*blank*/ + 3 /*footnotes*/
    // with a comment admitting the logo was "~7 rows" and that it used "the
    // taller estimate" deliberately. That over-count went straight into the
    // scroll ceiling, so `End` and the wheel ran 5-6 rows PAST the content and
    // you scrolled the help off the top into empty space — the exact mirror of
    // the detail panes, which were one row SHORT for the opposite reason.
    //
    // Both are now measured off the real element tree, so neither can drift
    // when the logo, a group or a footnote changes.
    [[nodiscard]] static int content_rows(int term_w) {
        using namespace maya;
        // Mirror the scroller's width math EXACTLY, or the measurement is of a
        // different layout than the one on screen and the ceiling drifts again:
        // the card's inner slot is the terminal minus the panel border and
        // padding, the scroller reserves 2 cells for its gutter + bar, and the
        // reading column is capped at the same design width build() passes
        // (150, wider than the domain panes' 104 — see build()).
        const int slot_w  = std::max(1, term_w - 4);
        const int gutter  = std::max(1, slot_w - 2);
        const int inner   = std::min(gutter, 150);
        long long total = 0;
        for (const Element& e : body_rows(term_w))
            total += std::max(1, measure_element(e, inner).height.value);
        return static_cast<int>(total);
    }

    // Viewport rows available for the body inside the full-frame card.
    //
    // The help card's chrome is 3 rows: the panel's top and bottom border,
    // plus the one-line hint bar. It is NOT the detail panes' 6 — those also
    // spend 3 on a system strip this overlay doesn't have, and copying their
    // number here claimed a viewport 3 rows shorter than the scroller actually
    // paints. Combined with the old over-estimated content tally, `End` ran
    // ~6 rows past the last line and you scrolled the reference off the top
    // into blank space.
    //
    // Must stay in step with build()'s own `view_h`, which is the number the
    // scroller really gets.
    [[nodiscard]] static int viewport_rows(int term_h) {
        return std::max(3, term_h - 3);
    }

    // The scrollable body, as the rows the scroller will window. Static and
    // width-only so content_rows() can MEASURE the very tree build() paints —
    // the whole reason the old hand-tallied estimate could drift from it.
    [[nodiscard]] static std::vector<maya::Element> body_rows(int width_) {
        using namespace maya;
        using namespace maya::dsl;

        // Full-screen, like htop's F1 / btop's help — the card fills the frame
        // and the key groups flow into TWO columns when the terminal is wide,
        // so the whole reference reads at a glance without scrolling.
        const bool two_col = width_ >= 92;
        const int keys_w = 15;

        // Render one group as a run of rows (header + entries + trailing gap).
        auto render_group = [&](const Group& g, std::vector<Element>& into) {
            into.push_back((h(
                text("▍", Style{}.with_fg(pal::sky)) | nowrap,
                text(g.name, Style{}.with_fg(pal::sky).with_bold()) | nowrap
            )).build());
            for (const auto& e : g.entries) {
                into.push_back((h(
                    text("  ") | nowrap,
                    text(e.keys, Style{}.with_fg(pal::text).with_bold())
                        | nowrap | width(keys_w),
                    text(e.desc, Style{}.with_fg(pal::label)) | clip | grow(1)
                ) | gap(1)).build());
            }
            into.push_back(blank());
        };

        std::vector<Element> header;
        // The wordmark splash — full block slab on a wide card, compact mark
        // when the terminal can't hold it without wrapping.
        header.push_back(Element{Logo{width_ < Logo::kFullWidth + 8}});
        header.push_back(blank());

        Element groups_block;
        if (two_col) {
            // Split the groups into two balanced columns. SORTING/GENERAL/MOUSE
            // are short; keep PROCESSES + DETAIL (the big ones) on the left.
            const auto& gs = groups();
            std::vector<Element> left, right;
            std::size_t split = (gs.size() + 1) / 2;
            for (std::size_t i = 0; i < gs.size(); ++i)
                render_group(gs[i], i < split ? left : right);
            groups_block = (h(
                v(std::move(left))  | grow(1),
                Element{blank()} | width(4),
                v(std::move(right)) | grow(1)
            )).build();
        } else {
            std::vector<Element> col;
            for (const auto& g : groups()) render_group(g, col);
            groups_block = (v(std::move(col))).build();
        }

        std::vector<Element> body;
        body.push_back((v(std::move(header))).build());
        body.push_back(std::move(groups_block));
        body.push_back(blank());
        body.push_back((text("Banner: green calm · blue busy · orange stressed · red critical.")
                        | nowrap | fgc(pal::dim)).build());
        body.push_back((text("stall chips = kernel PSI: % of time tasks waited on a resource.")
                        | nowrap | fgc(pal::dim)).build());
        body.push_back((text("▁▅█ tree fold marker = subtree CPU  ·  » culprit  ·  ▎ selection.")
                        | nowrap | fgc(pal::dim)).build());

        return body;
    }

private:
    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> body = body_rows(width_);

        // Window through the shared scroller (only bites on a very short term),
        // then a hint bar, all inside a full-frame card — the detail-pane idiom.
        // viewport_rows() owns this number so the app's scroll clamp and the
        // scroller can't disagree about how tall the window is.
        const int view_h = viewport_rows(height_);
        // A wider design cap than the domain panes (150 vs 104): the help is a
        // two-column reference table whose descriptions run ~50 cols each, so
        // the 104 cap would clip them. 150 lets both columns print in full,
        // and the scroller still CENTERS the capped block so a 240-col
        // terminal shows symmetric margin, not a stretched sparse table.
        Element windowed = detail::scroller(std::move(body), scroll_, view_h,
                                            pal::sky, /*cap_width=*/true,
                                            /*design_w=*/150);
        Element hintbar = (h(
            text(" esc") | nowrap | Bold | fgc(pal::sky),
            text("·close   ") | nowrap | fgc(pal::dim),
            text("↑↓") | nowrap | Bold | fgc(pal::sky),
            text("·scroll") | nowrap | fgc(pal::dim)
        )).build();

        Element card = Panel("?", "HELP", pal::sky).grow(1)(
            {std::move(windowed), std::move(hintbar)});
        return (v(std::move(card) | grow(1)) | grow(1)).build();
    }
};

}  // namespace rockbottom::ui
