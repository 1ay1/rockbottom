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
                {"t", "toggle FLOW tree ↔ flat list"},
                {"← →", "collapse / expand subtree (flow)"},
                {"= / +", "collapse-all / expand-all (flow)"},
                {"▁▅█ gutter", "CPU share vs siblings — tallest/brightest = the hog"},
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
                {"↑↓ / PgUp PgDn", "walk the list / scroll the pane"},
                {"← →", "proc pane: walk to parent / busiest child"},
                {"r", "proc pane: renice this process"},
                {"T", "proc pane: end this process + its whole subtree"},
                {"x K l X", "proc pane: stop · kill · signal · end-all-by-name"},
                {"g / G", "jump to top / bottom of pane"},
                {"Esc", "close detail / help"},
            }},
            {"GENERAL", {
                {"p / Space", "pause / resume"},
                {"< / >", "slower / faster refresh (250ms–5s)"},
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
    // scroll offset. In two-column mode the groups render side-by-side, so the
    // body is roughly the taller column; single-column stacks them all.
    [[nodiscard]] static int content_rows(int term_w) {
        const bool two_col = term_w >= 92;
        int rows_each = 0;
        std::vector<int> heights;
        for (const auto& g : groups())
            heights.push_back(1 + static_cast<int>(g.entries.size()) + 1);
        int total = 0; for (int h : heights) total += h;
        int body;
        if (two_col) {
            std::size_t split = (heights.size() + 1) / 2;
            int left = 0, right = 0;
            for (std::size_t i = 0; i < heights.size(); ++i)
                (i < split ? left : right) += heights[i];
            body = std::max(left, right);
        } else {
            body = total;
        }
        (void)rows_each;
        return 2 /*tagline+blank*/ + body + 1 /*blank*/ + 3 /*footnotes*/;
    }

    // Viewport rows available for the body inside the full-frame card:
    // outer grow + panel border(2) + panel padding(2) + hint bar(1) + slack.
    [[nodiscard]] static int viewport_rows(int term_h) {
        return std::max(3, term_h - 6);
    }

private:
    [[nodiscard]] maya::Element build() const {
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
                    text(e.desc, Style{}.with_fg(pal::label)) | nowrap
                ) | gap(1)).build());
            }
            into.push_back(blank());
        };

        std::vector<Element> header;
        header.push_back((text("A calmer system monitor — it tells you what's happening.")
                          | nowrap | fgc(pal::label)).build());
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

        // Window through the shared scroller (only bites on a very short term),
        // then a hint bar, all inside a full-frame card — the detail-pane idiom.
        const int view_h = std::max(3, height_ - 6);
        Element windowed = detail::scroller(std::move(body), scroll_, view_h, pal::sky);
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
