// widgets/detail/common.hpp — shared building blocks for every detail pane.
//
// The detail panes are bottom's "go deeper" surface: full-screen, per-domain
// drill-downs that carry MORE detail than htop or btop. Every number gets room
// to breathe, real graphs get a y-axis, and plain-language verdicts read the
// numbers *for* you. This header holds the primitives each per-domain file
// composes from — plus the RESPONSIVE context and the SCROLLER that keeps
// dense panes usable at any terminal size.

#pragma once

#include <maya/maya.hpp>

#include "../../../core/metrics.hpp"
#include "../../fmt.hpp"
#include "../../theme.hpp"
#include "../panel.hpp"
#include "../graph.hpp"
#include "../spark.hpp"
#include "../meter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

namespace rockbottom::ui::detail {

using maya::Element;

// Responsive context handed to every pane. `w`/`h` are the *terminal* size;
// panes use them to drop columns, shrink graphs, and pick densities. `scroll`
// is the body row offset (managed by the app). `body_h` is how many content
// rows fit inside the framed viewport (== h - chrome).
struct Ctx {
    int  w = 100;
    int  h = 40;
    int  scroll = 0;
    // Derived responsive flags, filled by make().
    bool wide = true;      // room for multi-column / side-by-side layouts
    bool ultrawide = false;// room to lay the WHOLE pane out in two columns
    bool tall = true;      // room for the big graphs
    int  graph_h = 8;      // rows available for the hero graph
    int  body_h = 30;      // scrollable viewport height

    static Ctx make(int w, int h, int scroll) {
        Ctx c;
        c.w = w; c.h = h; c.scroll = std::max(0, scroll);
        c.wide = w >= 84;
        // On a big monitor a single tall column wastes 2/3 of the width and
        // leaves the hero graph a thin line in an empty sky. Panes that opt in
        // reflow into two side-by-side columns above this width.
        c.ultrawide = w >= 170;
        c.tall = h >= 30;
        // Frame chrome: panel border(2) + panel padding(2) + hint(1) = 5 rows.
        c.body_h = std::max(3, h - 5);
        // Hero graph height: keep it MODERATE so a low-load trace still reads.
        // In two-column mode the columns share the height, so the graph can be
        // a touch shorter; either way it never balloons to fill a tall screen.
        c.graph_h = c.ultrawide ? std::clamp(h - 24, 5, 9)
                                : std::clamp(h - 22, 5, 10);
        return c;
    }
};

// ── content primitives ───────────────────────────────────────────────────

// A label : value row — label dim + fixed width, value bold + colored.
// Same 14-col label rail + 2-col gutter as bar() and kv3, so single rows
// and stat strips share their left edge.
inline Element kv(const std::string& k, const std::string& v, maya::Color vc,
                  int kw = 14) {
    using namespace maya; using namespace maya::dsl;
    return (h(
        text(k) | nowrap | fgc(pal::dim) | width(kw),
        text(v) | nowrap | Bold | fgc(vc)
    ) | gap(2)).build();
}

// Up to three label:value pairs on one line — dense stat strips. Empty key =
// blank spacer cell (lets callers show 1 or 2 pairs too). The row is split
// into three EQUAL FIXED columns (not grow(1) — maya flex distributes by
// content size, so a long value in one row would shove its column out of
// line with the rows above). Labels get the same 14-col width as bar()/kv(),
// so every label and every value in a pane sits on the same two rails.
inline Element kv3(std::string k1, std::string v1, maya::Color c1,
                   std::string k2 = "", std::string v2 = "", maya::Color c2 = pal::dim,
                   std::string k3 = "", std::string v3 = "", maya::Color c3 = pal::dim) {
    using namespace maya;
    struct Cell { std::string k, v; maya::Color c; };
    std::array<Cell, 3> cells{Cell{std::move(k1), std::move(v1), c1},
                              Cell{std::move(k2), std::move(v2), c2},
                              Cell{std::move(k3), std::move(v3), c3}};
    return Element{ComponentElement{
        .render = [cells](int w, int) -> Element {
            using namespace maya::dsl;
            const int cw = std::max(20, w / 3);
            std::vector<Element> row;
            for (const auto& cell : cells) {
                if (cell.k.empty()) {
                    row.push_back((Element{blank()} | width(cw)).build());
                    continue;
                }
                row.push_back((h(
                    text(cell.k) | nowrap | fgc(pal::dim) | width(14),
                    text(cell.v) | nowrap | Bold | fgc(cell.c)
                ) | gap(2) | width(cw)).build());
            }
            return (h(std::move(row))).build();
        },
    }};
}

// A full-width labelled meter row: "label  ██████░░  42%  <tail>".
// `tail_w` shrinks on narrow terminals (caller passes ctx.wide ? 34 : 0).
inline Element bar(const std::string& label, double frac,
                   const std::string& tail, maya::Color c, int tail_w = 34) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> row;
    row.push_back((text(label) | nowrap | fgc(pal::label) | width(14)).build());
    row.push_back((Element{Meter{frac}.fill().color(c)} | grow(1)).build());
    row.push_back((text(fmt::pct_pad(frac)) | nowrap | Bold | fgc(load_color(frac)) | width(5) | justify(Justify::End)).build());
    if (tail_w > 0)
        row.push_back((text(tail) | nowrap | fgc(pal::dim) | width(tail_w)).build());
    return (h(std::move(row)) | gap(2)).build();
}

// A section heading in the house style shared with the help overlay: a solid
// accent ▍ bar, the name in bold accent ink, then a hairline rule trailing
// off to the right edge so the heading both READS as a title and separates
// the block below. An optional right-aligned chip carries a stat for the
// section ("TOP MEMORY CONSUMERS   8 shown") without a second line.
inline Element section(std::string title, maya::Color ac, std::string chip = "") {
    using namespace maya; using namespace maya::dsl;
    std::string t = std::move(title);
    std::string ch = std::move(chip);
    return Element{ComponentElement{
        .render = [t, ac, ch](int w, int) -> Element {
            const maya::Color rule = mix(pal::border, ac, 0.35);
            // The chip is the priority on the right; the rule fills whatever
            // is left between the name and the chip. A 1-cell right inset
            // keeps the rule from kissing the panel border.
            const int avail = std::max(0, w - 1);
            std::string content = "▍ ";                       // accent bar + gap
            std::vector<StyledRun> runs;
            runs.push_back({0, content.size(), Style{}.with_fg(ac)});
            std::size_t off = content.size();
            content += t;
            runs.push_back({off, t.size(), Style{}.with_bold().with_fg(ac)});

            // Reserve room for a trailing chip ( " · <chip>" ) on the right.
            const int chip_cells = ch.empty() ? 0
                                 : 3 + static_cast<int>(ch.size());       // " · " + chip
            const int used = 2 + static_cast<int>(t.size()) + 1;         // bar + name + gap
            const int rule_cells = std::max(0, avail - used - chip_cells);

            off = content.size();
            std::string rulestr = " ";
            for (int i = 0; i < rule_cells; ++i) rulestr += "─";
            content += rulestr;
            runs.push_back({off, rulestr.size(), Style{}.with_fg(rule)});

            if (!ch.empty()) {
                off = content.size();
                std::string sep = " · ";
                content += sep;
                runs.push_back({off, sep.size(), Style{}.with_fg(rule)});
                off = content.size();
                content += ch;
                runs.push_back({off, ch.size(), Style{}.with_fg(mix(ac, pal::text, 0.35))});
            }
            return Element{TextElement{.content = std::move(content), .style = {},
                                       .wrap = TextWrap::NoWrap, .runs = std::move(runs)}};
        },
    }};
}

// A plain-language verdict line — bottom's signature "read it for you" touch.
// A colored ▌ gutter bar marks it as the pane's editorial voice, distinct
// from the data rows around it.
inline Element verdict(const std::string& msg, maya::Color c) {
    using namespace maya; using namespace maya::dsl;
    return (h(
        text(" ▌") | nowrap | fgc(c),
        text(msg) | nowrap | fgc(c)
    ) | gap(1)).build();
}

// A ranked top-N list row: " N  name  ████░░  value". The rank digit is a
// quiet ordinal — #1 gets the accent so the biggest consumer pops — and the
// grid (rank 3 / name / meter / figures) is identical across panes, so once
// you've read one "top" list you've read them all.
inline Element rank_row(int rank, const std::string& pid, const std::string& name,
                        double frac, maya::Color ac,
                        const std::string& v1, maya::Color c1, int v1w,
                        const std::string& v2 = "", maya::Color c2 = pal::label, int v2w = 0) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> row;
    row.push_back((text(std::to_string(rank)) | nowrap
                   | fgc(rank == 1 ? ac : pal::faint) | width(2) | justify(Justify::End)).build());
    row.push_back((text(pid) | nowrap | fgc(pal::dim) | width(7) | justify(Justify::End)).build());
    Style nst = Style{}.with_fg(rank == 1 ? pal::white : pal::text);
    if (rank == 1) nst = nst.with_bold();
    row.push_back((text(name, nst) | nowrap | width(23)).build());
    row.push_back((Element{Meter{frac}.fill().groove(false).color(ac)} | grow(1)).build());
    row.push_back((text(v1) | nowrap | Bold | fgc(c1) | width(v1w) | justify(Justify::End)).build());
    if (v2w > 0)
        row.push_back((text(v2) | nowrap | fgc(c2) | width(v2w) | justify(Justify::End)).build());
    return (h(std::move(row)) | gap(1)).build();
}

// ── BIG-NUMBER STAT CARD ─────────────────────────────────────────────
// The Grafana "stat panel" idiom, terminal-native: the pane's headline
// figure rendered in 3-row seven-segment block digits, with a trend arrow
// and window stats (avg / peak) beneath — parked LEFT of the hero graph so
// the one number you came for is unmissable from across the room.

inline const char* big_digit(int d, int row) {
    static constexpr const char* F[10][3] = {
        {"█▀█", "█ █", "█▄█"},   // 0
        {"▄█ ", " █ ", "▄█▄"},   // 1
        {"▀▀█", "█▀▀", "█▄▄"},   // 2
        {"▀▀█", " ▀█", "▄▄█"},   // 3
        {"█ █", "▀▀█", "  █"},   // 4
        {"█▀▀", "▀▀█", "▄▄█"},   // 5
        {"█▀▀", "█▀█", "█▄█"},   // 6
        {"▀▀█", "  █", "  █"},   // 7
        {"█▀█", "█▀█", "█▄█"},   // 8
        {"█▀█", "▀▀█", "▄▄█"},   // 9
    };
    return F[d][row];
}

// A fixed-width column: big percent digits, label, trend arrow computed from
// the history ring (recent window vs the stretch before it), avg + peak of
// the window. Emits exactly `rows_avail` rows so it slots beside a graph of
// the same height; lines drop from the bottom when the graph is short.
inline Element stat_card(double frac, maya::Color c, const std::string& label,
                         const float* hist, int len, int rows_avail) {
    using namespace maya; using namespace maya::dsl;
    const int pct = std::clamp(static_cast<int>(std::lround(frac * 100)), 0, 999);
    std::string digits = std::to_string(pct);

    std::array<std::string, 3> rows;
    for (char ch : digits)
        for (int r = 0; r < 3; ++r)
            rows[static_cast<std::size_t>(r)] += std::string(big_digit(ch - '0', r)) + " ";
    rows[2] += "%";

    // Window stats + trend: mean of the last ~6 samples vs the ~18 before.
    double avg = 0, peak = 0, recent = 0, prior = 0;
    int rn = 0, pn = 0;
    for (int i = 0; i < len; ++i) {
        const double v = hist[i];
        avg += v; peak = std::max(peak, v);
        if (i >= len - 6) { recent += v; ++rn; }
        else if (i >= len - 24) { prior += v; ++pn; }
    }
    if (len > 0) avg /= len;
    if (rn) recent /= rn;
    if (pn) prior /= pn;
    const double d = pn ? recent - prior : 0;
    const char* arrow = d > 0.03 ? "↗ rising" : d < -0.03 ? "↘ falling" : "→ steady";
    const maya::Color ac = d > 0.03 ? pal::hot : d < -0.03 ? pal::good : pal::dim;

    std::vector<Element> col;
    for (int r = 0; r < 3 && r < rows_avail; ++r)
        col.push_back((text(rows[static_cast<std::size_t>(r)]) | nowrap | Bold | fgc(c)).build());
    if (rows_avail >= 5)
        col.push_back((text(label) | nowrap | fgc(pal::dim)).build());
    if (rows_avail >= 6)
        col.push_back((text(arrow) | nowrap | fgc(ac)).build());
    if (rows_avail >= 8) {
        col.push_back((text("avg " + fmt::pct(avg)) | nowrap | fgc(pal::faint)).build());
        col.push_back((text("pk  " + fmt::pct(peak)) | nowrap | fgc(pal::faint)).build());
    }
    while (static_cast<int>(col.size()) < rows_avail) col.push_back(blank());
    return (v(std::move(col)) | width(16)).build();
}

// ── STACKED COMPOSITION BAR ──────────────────────────────────────────
// One full-width bar whose colored segments show HOW a total is composed
// (the Activity-Monitor / htop memory idiom) — far more legible than a
// stack of near-identical meters. Free space renders as a quiet ░ tail.
struct Seg { double frac; maya::Color c; };

inline Element comp_bar(std::vector<Seg> segs) {
    using namespace maya;
    return Element{ComponentElement{
        .render = [segs = std::move(segs)](int w, int) -> Element {
            std::string content;
            std::vector<StyledRun> runs;
            int used = 0;
            for (const Seg& s : segs) {
                int cells = static_cast<int>(std::lround(s.frac * w));
                cells = std::min(cells, w - used);
                if (cells <= 0) continue;
                std::size_t off = content.size();
                for (int i = 0; i < cells; ++i) content += "█";
                runs.push_back({off, content.size() - off, Style{}.with_fg(s.c)});
                used += cells;
            }
            if (used < w) {
                std::size_t off = content.size();
                for (int i = used; i < w; ++i) content += "░";
                runs.push_back({off, content.size() - off,
                                Style{}.with_fg(mix(pal::dim, pal::bg_panel, 0.5))});
            }
            return Element{TextElement{.content = std::move(content), .style = {},
                                       .wrap = TextWrap::NoWrap, .runs = std::move(runs)}};
        },
    }};
}

// Legend row for a comp_bar: "■ apps 5.5G   ■ wired 2.2G   …" — swatch in
// the segment color, label dim, value bold.
struct LegendItem { std::string label, value; maya::Color c; };

inline Element comp_legend(const std::vector<LegendItem>& items) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> row;
    for (const LegendItem& it : items) {
        row.push_back((text("■") | nowrap | fgc(it.c)).build());
        row.push_back((text(" " + it.label + " ") | nowrap | fgc(pal::dim)).build());
        row.push_back((text(it.value) | nowrap | Bold | fgc(pal::label)).build());
        row.push_back((text("   ") | nowrap).build());
    }
    return (h(std::move(row))).build();
}

// Peak-normalize a raw-rate history (B/s floats) into 0..1 for Spark, which
// clamps samples to [0,1] — feeding it raw rates renders a solid wall.
// The peak has a 1 KiB/s floor: without it a 2 B/s noise blip on an idle
// interface normalizes to a FULL-HEIGHT spike next to a "pk 0B/s" caption —
// visually a lie. Under the floor, everything reads as the flatline it is.
inline std::array<float, 48> norm48(const float* h, int len, float* peak_out = nullptr) {
    float peak = 1024.0f;
    for (int i = 0; i < len && i < 48; ++i) peak = std::max(peak, h[i]);
    std::array<float, 48> out{};
    for (int i = 0; i < len && i < 48; ++i) out[static_cast<std::size_t>(i)] = h[i] / peak;
    if (peak_out) *peak_out = peak;
    return out;
}

// Peak-normalize an already-0..1 history ring (per-process cpu%) so a quiet
// process still shows shape, while a busy one fills the spark. `floor` keeps a
// near-idle series from amplifying sampling noise into a full-height wall.
inline std::array<float, 48> norm_unit(const float* h, int len, float floor,
                                       float* peak_out = nullptr) {
    float peak = floor;
    for (int i = 0; i < len && i < 48; ++i) peak = std::max(peak, h[i]);
    std::array<float, 48> out{};
    if (peak <= 0) peak = 1.0f;
    for (int i = 0; i < len && i < 48; ++i)
        out[static_cast<std::size_t>(i)] = std::clamp(h[i] / peak, 0.0f, 1.0f);
    if (peak_out) *peak_out = peak;
    return out;
}

// A blank spacer row.
inline Element gap_row() {
    using namespace maya; using namespace maya::dsl;
    return blank();
}

// ── TWO-COLUMN BODY ──────────────────────────────────────────────────
// Compose two independent row-lists side by side, each in its own vertical
// stack, with a gutter between. Used by panes in ultrawide mode to spend the
// horizontal room instead of scrolling a single tall column. The taller list
// sets the row height; the shorter one just leaves space below it. Returns a
// SINGLE body row (the h-stack) so the scroller treats the whole split as one
// unit — panes stay scroll-safe because two_col output is short by design.
inline std::vector<Element> two_col(std::vector<Element> left,
                                    std::vector<Element> right) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> out;
    out.push_back((h(
        v(std::move(left))  | grow(1),
        text("  ") | nowrap,
        v(std::move(right)) | grow(1)
    ) | gap(2)).build());
    return out;
}

// ── SCROLLER ──────────────────────────────────────────────────────────────────────────────
// Take a full body (may be taller than the viewport) and window it to the
// slot the layout actually hands us. `scroll` indexes ELEMENTS (the app's
// scroll unit); the window packs elements until their MEASURED heights fill
// the real slot — a 10-row hero graph counts as 10 rows, not 1, so nothing
// is crushed by flex-shrink and nothing bleeds past the frame into the hint
// bar. Heights come from measure_element over the real fragments at the
// real width; the proportional scrollbar runs on rows, not element counts.
inline Element scroller(std::vector<Element> body, int scroll, int /*view_h*/,
                        maya::Color ac) {
    using namespace maya; using namespace maya::dsl;
    auto shared = std::make_shared<const std::vector<Element>>(std::move(body));
    const int scroll_in = std::max(0, scroll);
    maya::ComponentElement ce{
        .render = [shared, scroll_in, ac](int slot_w, int slot_h) -> Element {
            const auto& all = *shared;
            const int total = static_cast<int>(all.size());
            const int view_h = std::max(1, slot_h);
            const int inner_w = std::max(1, slot_w - 2);   // minus " "+bar gutter

            // Measure every element at the real width — the same fragments
            // that will paint, so window and paint can never disagree.
            std::vector<int> rows(static_cast<std::size_t>(total), 1);
            long long total_rows = 0;
            for (int i = 0; i < total; ++i) {
                rows[static_cast<std::size_t>(i)] = std::max(1,
                    measure_element(all[static_cast<std::size_t>(i)], inner_w)
                        .height.value);
                total_rows += rows[static_cast<std::size_t>(i)];
            }

            if (total_rows <= view_h) {
                std::vector<Element> win(all.begin(), all.end());
                return (v(std::move(win)) | grow(1)).build();
            }

            // Last useful start element: the first index from which the tail
            // still fills the viewport — scrolling past it just shows the
            // same last page.
            int max_start = total - 1;
            {
                long long tail = 0;
                for (int i = total - 1; i >= 0; --i) {
                    tail += rows[static_cast<std::size_t>(i)];
                    if (tail >= view_h) { max_start = i; break; }
                    max_start = i;
                }
            }
            const int start = std::clamp(scroll_in, 0, max_start);

            // Pack elements until the measured rows fill the slot.
            std::vector<Element> win;
            int used = 0;
            for (int i = start; i < total && used < view_h; ++i) {
                win.push_back(all[static_cast<std::size_t>(i)]);
                used += rows[static_cast<std::size_t>(i)];
            }

            // Scrollbar on ROW basis.
            long long rows_before = 0;
            for (int i = 0; i < start; ++i)
                rows_before += rows[static_cast<std::size_t>(i)];
            const long long max_scroll_rows =
                std::max<long long>(1, total_rows - view_h);
            const int thumb = std::max(1,
                static_cast<int>(static_cast<long long>(view_h) * view_h
                                 / std::max<long long>(1, total_rows)));
            const int track = std::max(0, view_h - thumb);
            const int pos = static_cast<int>(
                std::min<long long>(rows_before, max_scroll_rows)
                * track / max_scroll_rows);
            std::vector<Element> barcol;
            for (int r = 0; r < view_h; ++r) {
                const bool on = r >= pos && r < pos + thumb;
                const char* g = r == 0 && start > 0 ? "▲"
                              : r == view_h - 1 && start < max_start ? "▼"
                              : on ? "█" : "│";
                barcol.push_back((text(g) | nowrap | fgc(on ? ac : pal::faint)).build());
            }

            return (h(
                v(std::move(win)) | grow(1),
                text(" ") | nowrap,
                v(std::move(barcol)) | width(1)
            )).build();
        },
        // Fill semantics: full slot width, 1-row basis — grow (baked on the
        // component, maya fill() contract) expands the height. (A measured
        // leaf in a COLUMN takes its width from measure, not cross-stretch.)
        .measure = [](int max_width) -> maya::Size {
            return {maya::Columns{max_width > 0 ? max_width : 1}, maya::Rows{1}};
        },
    };
    ce.layout.grow = 1.0f;
    return Element{std::move(ce)};
}

// Total content rows for a body — used by the app to clamp scroll offsets.
// (Panes expose their body length via the DetailPane::content_rows API.)

}  // namespace rockbottom::ui::detail
