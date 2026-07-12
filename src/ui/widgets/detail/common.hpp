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
        // reflow into two side-by-side columns above this width. Two ~72-col
        // reading columns + a 2-col gutter fit from ~146 cols; splitting there
        // (rather than 170) kills the lopsided right-hand VOID a single
        // 104-capped column leaves on a 150-col terminal.
        c.ultrawide = w >= 146;
        c.tall = h >= 30;
        // Frame chrome: panel border(2) + panel padding(2) + hint(1) = 5 rows.
        c.body_h = std::max(3, h - 5);
        // Hero graph height. On a SHORT pane keep it moderate so a low-load
        // trace still reads and the sections below it stay above the fold.
        // On a TALL pane, though, a fixed ~9-row graph strands 20+ empty rows
        // beneath the content (content-light panes like DISK/GPU used barely
        // a third of a 50-row screen). A full-width area graph reads BETTER
        // with more vertical resolution, not worse — the "thin line in an
        // empty sky" risk is about WIDTH, and these graphs span the pane. So
        // let the graph claim a share of the surplus height above a ~30-row
        // baseline, capped so it never eats the whole screen. In two-column
        // mode the columns share height, so bias a touch shorter.
        const int base = c.ultrawide ? std::clamp(h - 24, 5, 9)
                                     : std::clamp(h - 22, 5, 10);
        const int surplus = std::max(0, h - 30);
        c.graph_h = std::min(22, base + surplus / 2);
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
// RESPONSIVE: below ~78 cols three 26-cell columns can't hold label+value,
// so the strip reflows — two pairs per row at medium widths, one per row
// when truly cramped (the scroller measures real heights, so the extra rows
// are windowed correctly). Values are clip-truncated either way: a
// pathological value ellipsizes instead of colliding with its neighbour.
inline Element kv3(std::string k1, std::string v1, maya::Color c1,
                   std::string k2 = "", std::string v2 = "", maya::Color c2 = pal::dim,
                   std::string k3 = "", std::string v3 = "", maya::Color c3 = pal::dim) {
    using namespace maya;
    struct Cell { std::string k, v; maya::Color c; };
    std::array<Cell, 3> cells{Cell{std::move(k1), std::move(v1), c1},
                              Cell{std::move(k2), std::move(v2), c2},
                              Cell{std::move(k3), std::move(v3), c3}};
    // Columns the width can hold: 3 × 26 / 2 × 26 / take what you get.
    auto cols_for = [](int w) { return w >= 78 ? 3 : w >= 52 ? 2 : 1; };
    auto live = [](const std::array<Cell, 3>& cs) {
        int n = 0;
        for (const auto& c : cs) if (!c.k.empty()) ++n;
        return std::max(1, n);
    };
    // A label:value pair reads at ~30 cells (14 rail + 2 gap + ~14 value).
    // CAP the column at this — on a 200-col ultrawide, w/3 would stretch each
    // pair across ~66 cells, floating the value ~50 cells from its label and
    // opening yawning gulfs between the three pairs (they'd read as scattered
    // debris, not one strip). Instead the pairs stay grouped tight and the
    // surplus width becomes quiet trailing space — the eye tracks a compact
    // block, not a sparse line. A trailing grow(1) spacer absorbs the slack.
    constexpr int kColCap = 30;
    maya::ComponentElement ce{
        .render = [cells, cols_for, kColCap](int w, int) -> Element {
            using namespace maya::dsl;
            const int cols = cols_for(w);
            const int cw = std::clamp(w / cols, 20, kColCap);
            std::vector<Element> lines;
            std::vector<Element> row;
            int in_row = 0;
            auto flush = [&] {
                // Left-anchor the grouped pairs; slack goes to a trailing
                // spacer so pairs never drift apart across a wide pane.
                row.push_back((Element{blank()} | grow(1)).build());
                lines.push_back((h(std::move(row))).build());
                row.clear(); in_row = 0;
            };
            for (std::size_t i = 0; i < cells.size(); ++i) {
                const auto& cell = cells[i];
                // Skip blank spacers when reflowed — they only exist to hold
                // the 3-column grid; in 2/1-col mode they'd waste a slot.
                if (cell.k.empty()) {
                    if (cols == 3)
                        row.push_back((Element{blank()} | width(cw)).build());
                    continue;
                }
                row.push_back((h(
                    text(cell.k) | nowrap | fgc(pal::dim) | width(14),
                    text(cell.v) | clip | Bold | fgc(cell.c) | grow(1)
                ) | gap(2) | width(cw)).build());
                if (++in_row == cols) flush();
            }
            if (!row.empty()) flush();
            if (lines.empty()) lines.push_back(blank());
            if (lines.size() == 1) return std::move(lines.front());
            return (v(std::move(lines))).build();
        },
        // Height depends on the slot width (pairs ÷ columns) — report it so
        // the detail scroller's row-aware window stays exact.
        .measure = [cells, cols_for, live](int max_width) -> maya::Size {
            const int cols = cols_for(max_width > 0 ? max_width : 100);
            const int n = live(cells);
            const int rows = (n + cols - 1) / cols;
            return {maya::Columns{max_width > 0 ? max_width : 1},
                    maya::Rows{std::max(1, rows)}};
        },
    };
    return Element{std::move(ce)};
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
            // If the pane is too thin to hold the name AND the chip, drop the
            // chip WHOLE — a clipped "· t" stub reads as garbage.
            const int used = 2 + static_cast<int>(t.size()) + 1;         // bar + name + gap
            int chip_cells = ch.empty() ? 0
                           : 3 + static_cast<int>(ch.size());            // " · " + chip
            const bool with_chip = chip_cells > 0 && used + chip_cells <= avail;
            if (!with_chip) chip_cells = 0;
            const int rule_cells = std::max(0, avail - used - chip_cells);

            off = content.size();
            std::string rulestr = " ";
            for (int i = 0; i < rule_cells; ++i) rulestr += "─";
            content += rulestr;
            runs.push_back({off, rulestr.size(), Style{}.with_fg(rule)});

            if (with_chip) {
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
// from the data rows around it. The message clip-truncates with … on narrow
// terminals instead of hard-cutting at the frame.
inline Element verdict(const std::string& msg, maya::Color c) {
    using namespace maya; using namespace maya::dsl;
    return (h(
        text(" ▌") | nowrap | fgc(c),
        text(msg) | clip | fgc(c) | grow(1)
    ) | gap(1)).build();
}

// A ranked top-N list row: " N  name  ████░░  value". The rank digit is a
// quiet ordinal — #1 gets the accent so the biggest consumer pops — and the
// grid (rank 3 / name / meter / figures) is identical across panes, so once
// you've read one "top" list you've read them all.
// Width-aware: the full grid needs ~45 cols; on a thin pane flex used to
// clip the fixed cells' DIGITS ("13683" for pid 136830, "6.0" losing its %)
// — numbers that lie. Instead shed by priority: v2 → shrink name → meter →
// pid; the rank, a readable name, and the primary value always survive.
inline Element rank_row(int rank, const std::string& pid, const std::string& name,
                        double frac, maya::Color ac,
                        const std::string& v1, maya::Color c1, int v1w,
                        const std::string& v2 = "", maya::Color c2 = pal::label, int v2w = 0) {
    using namespace maya;
    return Element{maya::ComponentElement{
        .render = [=](int w, int) -> Element {
            using namespace maya::dsl;
            constexpr int kGap = 1, kMeterMin = 6;
            bool keep_v2    = v2w > 0;
            bool keep_meter = true;
            bool keep_pid   = true;
            int  name_w     = 23;
            auto need = [&] {
                return 2 + kGap + (keep_pid ? 7 + kGap : 0) + name_w + kGap
                     + (keep_meter ? kMeterMin + kGap : 0) + v1w
                     + (keep_v2 ? kGap + v2w : 0);
            };
            if (need() > w) keep_v2 = false;
            if (need() > w) name_w = std::max(10, name_w - (need() - w));
            if (need() > w) keep_meter = false;
            if (need() > w) keep_pid = false;
            if (need() > w) name_w = std::max(4, name_w - (need() - w));
            std::vector<Element> row;
            row.push_back((text(std::to_string(rank)) | nowrap
                           | fgc(rank == 1 ? ac : pal::faint) | width(2) | justify(Justify::End)).build());
            if (keep_pid)
                row.push_back((text(pid) | nowrap | fgc(pal::dim) | width(7) | justify(Justify::End)).build());
            Style nst = Style{}.with_fg(rank == 1 ? pal::white : pal::text);
            if (rank == 1) nst = nst.with_bold();
            // truncate_end (display-width, UTF-8-safe) — NOT fmt::clip, whose
            // byte-indexed substr can split a multi-byte codepoint mid-sequence
            // and render a replacement glyph.
            const std::string nm = static_cast<int>(string_width(name)) > name_w
                ? truncate_end(name, name_w) : name;
            row.push_back((text(nm, nst) | nowrap | width(name_w)).build());
            if (keep_meter)
                row.push_back((Element{Meter{frac}.fill().groove(false).color(ac)} | grow(1)).build());
            row.push_back((text(v1) | nowrap | Bold | fgc(c1) | width(v1w) | justify(Justify::End)).build());
            if (keep_v2)
                row.push_back((text(v2) | nowrap | fgc(c2) | width(v2w) | justify(Justify::End)).build());
            return (h(std::move(row)) | gap(kGap)).build();
        },
    }};
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

// ── HERO GRAPH ───────────────────────────────────────────────
// The shared "big-number stat card + y-axis + filled mountain" hero used by
// the CPU / MEM / GPU panes. Width-aware: the block-digit stat card needs
// ~16 cols, so on a thin pane it's DROPPED and the trace (with its labelled
// y-axis) owns the full width instead of being crushed to a 6-cell sliver.
// Optional overlay draws a second series (RAM over CPU, VRAM over GPU).
inline Element hero_graph(double frac, maya::Color card_c, const char* label,
                          const float* hist, int hist_len, int gh,
                          std::optional<maya::Color> graph_c = std::nullopt,
                          const float* overlay = nullptr, int overlay_len = 0,
                          maya::Color overlay_c = pal::mem_ac,
                          double axis_top = 100.0, bool axis_pct = true,
                          int axis_w = 3) {
    using namespace maya;
    return Element{maya::ComponentElement{
        .render = [=](int w, int) -> Element {
            using namespace maya::dsl;
            std::vector<Element> row;
            // Card only when there's genuine room for it AND the trace.
            if (w >= 64)
                row.push_back(stat_card(frac, card_c, label, hist, hist_len, gh));
            row.push_back(y_axis(gh, axis_top, axis_w, axis_pct));
            Graph g{hist, hist_len};
            g.fill().rows(gh);
            if (graph_c) g.color(*graph_c);
            if (overlay) g.overlay(overlay, overlay_len, overlay_c);
            // build(), NOT build_fixed(): fill() zeroes cells_, and
            // build_fixed() with cells_==0 draws ZERO dot-columns — a dead,
            // empty graph at every size. build() wraps the fill-mode
            // component that resolves the real slot width at paint.
            row.push_back(Element{g.build()});
            return (h(std::move(row)) | gap(1) | height(gh)).build();
        },
    }};
}

// ── DUAL-SERIES TRAFFIC HERO ─────────────────────────────────────────
// The rx/tx (and disk read/write) hero: a filled mountain (`fill_c`) with a
// second series overlaid as a line (`over_c`), on a shared sqrt-curve axis
// labelled in byte rates. Both `fill`/`over` are 0..1 fractions of `axis_top`.
//
// CRITICAL: Graph.fill() defers its sample read to PAINT time (a component
// resolved against the real slot width). So the sample buffers must OUTLIVE
// the calling function — a caller's stack std::array would dangle and the
// graph would read freed memory (garbage trace / corruption). This helper
// OWNS copies of both series in shared_ptrs the render lambda captures, so
// callers can pass transient locals safely.
inline Element traffic_hero(const float* fill, const float* over, int len,
                            double axis_top, maya::Color fill_c,
                            maya::Color over_c, int gh, float gamma = 0.5f,
                            int axis_w = 5) {
    auto f = std::make_shared<std::array<float, 48>>();
    auto o = std::make_shared<std::array<float, 48>>();
    const int n = std::min(len, 48);
    for (int i = 0; i < n; ++i) {
        (*f)[static_cast<std::size_t>(i)] = fill ? fill[i] : 0.0f;
        (*o)[static_cast<std::size_t>(i)] = over ? over[i] : 0.0f;
    }
    return Element{maya::ComponentElement{
        .render = [f, o, n, axis_top, fill_c, over_c, gh, gamma, axis_w]
                  (int, int) -> Element {
            using namespace maya; using namespace maya::dsl;
            return (h(
                y_axis(gh, axis_top, axis_w, /*percent=*/false, gamma),
                Element{Graph{f->data(), n}.fill().rows(gh).color(fill_c).gamma(gamma)
                            .overlay(o->data(), n, over_c)} | grow(1)
            ) | gap(1) | height(gh)).build();
        },
    }};
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
// the segment color, label dim, value bold. Width-aware: when one line can't
// hold every item, the legend wraps onto extra rows (2 items per row) —
// letting flex shrink the nowrap cells instead would eat interior spaces
// ("■ buffers24K").
struct LegendItem { std::string label, value; maya::Color c; };

inline Element comp_legend(std::vector<LegendItem> items) {
    using namespace maya;
    auto cell_w = [](const LegendItem& it) {
        return 2 + static_cast<int>(it.label.size()) + 1
                 + static_cast<int>(it.value.size());
    };
    auto one_line_w = [cell_w](const std::vector<LegendItem>& v) {
        int w = 0;
        for (std::size_t i = 0; i < v.size(); ++i)
            w += cell_w(v[i]) + (i + 1 < v.size() ? 3 : 0);
        return w;
    };
    auto build_rows = [cell_w](const std::vector<LegendItem>& v, int w)
        -> std::vector<std::vector<LegendItem>> {
        std::vector<std::vector<LegendItem>> rows;
        std::vector<LegendItem> cur;
        int used = 0;
        for (const auto& it : v) {
            const int need = cell_w(it) + (cur.empty() ? 0 : 3);
            if (!cur.empty() && used + need > w) {
                rows.push_back(std::move(cur));
                cur.clear(); used = 0;
            }
            used += cell_w(it) + (cur.empty() ? 0 : 3);
            cur.push_back(it);
        }
        if (!cur.empty()) rows.push_back(std::move(cur));
        return rows;
    };
    maya::ComponentElement ce{
        .render = [items, build_rows](int w, int) -> Element {
            using namespace maya::dsl;
            std::vector<Element> out;
            for (auto& line : build_rows(items, std::max(10, w))) {
                std::vector<Element> row;
                for (std::size_t i = 0; i < line.size(); ++i) {
                    const auto& it = line[i];
                    row.push_back((text("■") | nowrap | fgc(it.c)).build());
                    row.push_back((text(" " + it.label + " ") | nowrap | fgc(pal::dim)).build());
                    row.push_back((text(it.value) | nowrap | Bold | fgc(pal::label)).build());
                    if (i + 1 < line.size())
                        row.push_back((text("   ") | nowrap).build());
                }
                out.push_back((h(std::move(row))).build());
            }
            if (out.empty()) out.push_back(blank());
            return (v(std::move(out))).build();
        },
        .measure = [items, one_line_w, build_rows](int max_width) -> maya::Size {
            const int rows = max_width > 0
                ? static_cast<int>(build_rows(items, std::max(10, max_width)).size())
                : 1;
            return {maya::Columns{std::min(max_width > 0 ? max_width : one_line_w(items),
                                           one_line_w(items))},
                    maya::Rows{std::max(1, rows)}};
        },
    };
    return Element{std::move(ce)};
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

// ── WIDTH-AWARE THROUGHPUT ROW ───────────────────────────────────────
// The rx/tx (and disk read/write) strip: an arrow+label, a live sparkline
// that fills the slack, the current rate, and OPTIONAL trailing figures
// (packets/s, window peak, lifetime total). At full width all four show;
// as the pane narrows they SHED right-to-left — lifetime first, then peak,
// then pps — so the rate and spark stay readable instead of every column
// truncating into stubs ("pk 6", "↓ 9.", "0 p/"). Built as a component so
// the shed decision runs against the real solved width every frame.
struct FlowTail { std::string text; maya::Color color; int min_w; };

inline Element flow_row(const std::string& arrow_label, maya::Color label_c,
                        const float* hist, int hist_len, maya::Color spark_c,
                        const std::string& rate, maya::Color rate_c,
                        std::vector<FlowTail> tails = {}) {
    using namespace maya;
    std::array<float, 48> hn{};
    for (int i = 0; i < hist_len && i < 48; ++i)
        hn[static_cast<std::size_t>(i)] = std::clamp(hist[i], 0.0f, 1.0f);
    const int hlen = std::min(hist_len, 48);
    return Element{maya::ComponentElement{
        .render = [=](int w, int) -> Element {
            using namespace maya::dsl;
            constexpr int kLabelW = 7, kRateW = 10, kGap = 1, kSparkMin = 6;
            // Reserve label + rate + a minimal spark; whatever's left funds
            // the optional tails (shed right-to-left when they don't fit).
            int budget = w - kLabelW - kRateW - kSparkMin - 3 * kGap;
            std::vector<const FlowTail*> keep;
            for (const auto& t : tails) {
                if (budget >= t.min_w + kGap) { keep.push_back(&t); budget -= t.min_w + kGap; }
                else break;   // once one doesn't fit, drop the rest (order = priority)
            }
            std::vector<Element> row;
            row.push_back((text(arrow_label) | nowrap | fgc(label_c) | width(kLabelW)).build());
            row.push_back(Element{Spark{hn.data(), hlen}.fill().color(spark_c).baseline(true)} | grow(1));
            row.push_back((text(rate) | nowrap | Bold | fgc(rate_c) | width(kRateW) | justify(Justify::End)).build());
            for (const FlowTail* t : keep)
                row.push_back((text(t->text) | nowrap | fgc(t->color)
                               | width(std::max(t->min_w, static_cast<int>(string_width(t->text))))
                               | justify(Justify::End)).build());
            return (h(std::move(row)) | gap(kGap)).build();
        },
    }};
}

// ── TWO-COLUMN BODY ──────────────────────────────────────────────────
// Compose two independent row-lists side by side, each in its own vertical
// stack, with a gutter between. Used by panes in ultrawide mode to spend the
// horizontal room instead of scrolling a single tall column. The taller list
// sets the row height; the shorter one just leaves space below it. Returns a
// SINGLE body row (the h-stack) so the scroller treats the whole split as one
// unit — panes stay scroll-safe because two_col output is short by design.
//
// Each column is CAPPED at a comfortable reading width (kColDesign) rather
// than each taking half of a 200-col ultrawide: a 100-col-wide column still
// smears its graph and strands its meter values. The two capped columns sit
// left-anchored with the gutter between them; on an extremely wide screen
// the surplus becomes quiet right margin. Built as a component so the cap is
// resolved against the REAL slot width (and collapses gracefully when the
// slot can't even hold two design columns — then each just takes half).
inline std::vector<Element> two_col(std::vector<Element> left,
                                    std::vector<Element> right) {
    using namespace maya; using namespace maya::dsl;
    constexpr int kColDesign = 92;   // comfortable per-column reading width
    constexpr int kGutter    = 2;
    auto sl = std::make_shared<std::vector<Element>>(std::move(left));
    auto sr = std::make_shared<std::vector<Element>>(std::move(right));
    std::vector<Element> out;
    out.push_back(Element{maya::ComponentElement{
        .render = [sl, sr, kColDesign, kGutter](int w, int) -> Element {
            using namespace maya::dsl;
            // Column width: half the slot (minus gutter), capped at the design
            // measure so wider screens spill into right margin, not stretch.
            const int half = std::max(20, (w - kGutter) / 2);
            const int cw = std::min(half, kColDesign);
            std::vector<Element> lcol(sl->begin(), sl->end());
            std::vector<Element> rcol(sr->begin(), sr->end());
            Element cols = (h(
                v(std::move(lcol)) | width(cw),
                text("  ") | nowrap,
                v(std::move(rcol)) | width(cw)
            )).build();
            // On a genuinely ULTRA-wide screen the two capped columns would
            // otherwise sit pinned LEFT with the entire surplus dumped as a
            // right-hand VOID (>110 empty cols at 300w). CENTER the block so
            // the margin splits evenly and the content reads as a composed
            // slab, not debris shoved against the left edge. A modest surplus
            // still left-anchors (centering a near-full pair only jitters it).
            const int used = cw * 2 + kGutter;
            if (w - used >= 16)
                return (h(
                    Element{blank()} | grow(1),
                    std::move(cols),
                    Element{blank()} | grow(1)
                )).build();
            return (h(
                std::move(cols),
                Element{blank()} | grow(1)
            )).build();
        },
    }});
    return out;
}

// ── HERO-THEN-SPLIT BODY ─────────────────────────────────────────────
// The house layout for every drill-down: a BIG full-width hero (the domain's
// headline graph + its section title) owns the whole first band, then the
// detail reflows into two side-by-side columns beneath it. `hero` is the
// full-width rows (section + hero_graph, kept OUT of either column so the
// trace spans the entire pane instead of being crushed into a half-width
// sliver); `left`/`right` are the two_col lists. The hero rides at full slot
// width; a blank separates it from the columns. Returns the composed body
// (hero rows first, then the single two_col row) so the scroller windows the
// whole thing coherently.
inline std::vector<Element> hero_split(std::vector<Element> hero,
                                       std::vector<Element> left,
                                       std::vector<Element> right) {
    std::vector<Element> out = std::move(hero);
    out.push_back(gap_row());
    for (auto& e : two_col(std::move(left), std::move(right)))
        out.push_back(std::move(e));
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
                        maya::Color ac, bool cap_width = true, int design_w = 104) {
    using namespace maya; using namespace maya::dsl;
    auto shared = std::make_shared<const std::vector<Element>>(std::move(body));
    const int scroll_in = std::max(0, scroll);
    maya::ComponentElement ce{
        .render = [shared, scroll_in, ac, cap_width, design_w](int slot_w, int slot_h) -> Element {
            const auto& all = *shared;
            const int total = static_cast<int>(all.size());
            const int view_h = std::max(1, slot_h);
            const int gutter_w = std::max(1, slot_w - 2);  // minus " "+bar gutter

            // ── READABLE DESIGN WIDTH ──
            // Full-width content on a 200-col ultrawide reads badly: a load
            // graph smears across 90 cols into a flat dotted line, a meter's
            // groove runs the whole width with its value stranded far right,
            // section rules trail 90 dashes. Cap the CONTENT width at a
            // comfortable reading measure and left-anchor it — the surplus
            // becomes quiet right margin, NOT dead vertical space.
            //   Panes that reflowed into two side-by-side columns pass
            //   cap_width=false: two_col already caps each of its OWN columns
            //   at a reading width, so the scroller must hand it the full slot.
            constexpr int kDesignDefault = 104;   // comfortable single-column measure
            const int kDesign = design_w > 0 ? design_w : kDesignDefault;
            const int inner_w = cap_width ? std::min(gutter_w, kDesign) : gutter_w;

            // Measure every element at the SAME width we'll paint at — window
            // and paint can never disagree.
            std::vector<int> rows(static_cast<std::size_t>(total), 1);
            long long total_rows = 0;
            for (int i = 0; i < total; ++i) {
                rows[static_cast<std::size_t>(i)] = std::max(1,
                    measure_element(all[static_cast<std::size_t>(i)], inner_w)
                        .height.value);
                total_rows += rows[static_cast<std::size_t>(i)];
            }

            // Wrap the windowed column at the (possibly capped) width. When
            // the slot is much WIDER than the reading cap (a single-column
            // pane on a 130-150 col terminal that hasn't reached the two-col
            // split), CENTER the block so the surplus reads as symmetric
            // margin rather than a lopsided right-hand VOID beside the
            // scrollbar. A small surplus still left-anchors (centering a
            // near-full column just jitters it). The scrollbar always pins to
            // the far right via the outer h-stack in the caller.
            const int pad_w = std::max(0, gutter_w - inner_w);
            const bool center = pad_w >= 12;
            auto place = [&](std::vector<Element> col) -> Element {
                Element body = (v(std::move(col)) | width(inner_w)).build();
                if (pad_w <= 0) return (Element{std::move(body)} | grow(1)).build();
                if (center)
                    return (h(
                        Element{blank()} | grow(1),
                        Element{std::move(body)} | width(inner_w),
                        Element{blank()} | grow(1)
                    )).build();
                return (h(
                    Element{std::move(body)} | width(inner_w),
                    Element{blank()} | grow(1)
                )).build();
            };

            if (total_rows <= view_h) {
                std::vector<Element> win(all.begin(), all.end());
                return place(std::move(win));
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
                const char* g = r == 0 && start > 0 ? "\xe2\x96\xb2"
                              : r == view_h - 1 && start < max_start ? "\xe2\x96\xbc"
                              : on ? "\xe2\x96\x88" : "\xe2\x94\x82";
                barcol.push_back((text(g) | nowrap | fgc(on ? ac : pal::faint)).build());
            }

            return (h(
                place(std::move(win)) | grow(1),
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
