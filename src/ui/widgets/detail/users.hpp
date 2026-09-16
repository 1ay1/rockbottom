// widgets/detail/users.hpp — the USERS drill-down body.
//
// The admin pane. Every other view in rockbottom answers "which PROCESS", but
// on a shared box — a build farm, a jump host, a university server, a CI
// runner — the operative question is "which PERSON", and the follow-up is
// "show me their stuff" or "stop all of it". That normally means
// `ps -eo user,pcpu,rss --sort=-pcpu | awk ...`; here it's one keystroke.
//
// Layout mirrors the NET pane's split idiom: a fixed roster of headline stats
// on the left, the scrolling table on the right when there's width for it,
// stacked otherwise.

#pragma once

#include "common.hpp"
#include "../../user_stats.hpp"

#include <maya/widget/table.hpp>

namespace rockbottom::ui::detail {

// A user's share of a resource, as a fraction of the machine's total. Used for
// the inline meters — a bare "412%" means nothing without knowing the box has
// 12 cores, but a filled bar is instantly legible.
inline double share_of(double part, double whole) {
    return whole > 0 ? std::clamp(part / whole, 0.0, 1.0) : 0.0;
}

// A plain-text block bar sized to a solved column width. The pane's table
// cells are strings, not Elements, so the Meter widget can't be used inside
// one — this renders the same idea (█ filled, ░ track) as text. Sub-cell
// resolution via the eighth-block glyphs keeps a small share visible instead
// of rounding a real 3% down to an empty bar.
inline std::string bar_str(double frac, int w) {
    if (w <= 0) return {};
    frac = std::clamp(frac, 0.0, 1.0);
    static constexpr const char* kEighth[8] = {
        "", "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d",
        "\xe2\x96\x8c", "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89",
    };
    const int eighths = static_cast<int>(std::lround(frac * w * 8));
    const int full = std::min(w, eighths / 8);
    const int rem  = full < w ? eighths % 8 : 0;
    std::string out;
    for (int i = 0; i < full; ++i) out += "\xe2\x96\x88";      // █
    if (rem) out += kEighth[rem];
    int used = full + (rem ? 1 : 0);
    for (int i = used; i < w; ++i) out += "\xe2\x96\x91";       // ░ track
    return out;
}

inline std::vector<maya::ColumnDef> user_columns(bool wide) {
    using namespace maya;
    std::vector<ColumnDef> cols;
    cols.push_back({.header = "", .width = 2, .keep = kKeepAlways});   // pane indent
    cols.push_back({.header = "USER", .keep = kKeepAlways,
                    .weight = 2.0f, .min_width = 8, .max_width = 20});
    cols.push_back({.header = "CPU%", .keep = kKeepAlways, .min_width = 7});
    // The meters are what make this scannable, so they outrank the numeric
    // extras and only shed when the pane is genuinely cramped.
    if (wide) cols.push_back({.header = "", .keep = 4, .weight = 2.0f, .min_width = 8});
    cols.push_back({.header = "MEM", .keep = kKeepAlways, .min_width = 7});
    if (wide) cols.push_back({.header = "", .keep = 5, .weight = 2.0f, .min_width = 8});
    cols.push_back({.header = "PROCS", .keep = 1, .min_width = 5});
    cols.push_back({.header = "THR", .keep = 3, .min_width = 4});
    cols.push_back({.header = "I/O", .keep = 2, .min_width = 8});
    cols.push_back({.header = "BUSIEST", .keep = 6,
                    .weight = 2.6f, .min_width = 10, .max_width = 28});
    return cols;
}

// One table row for a user. `sel` marks the cursor row: an admin is about to
// press a key that signals everything this row owns, so which row is armed has
// to be unmistakable, not a subtle tint.
inline maya::TableRow user_row(const UserStat& u, double total_cpu,
                               std::uint64_t total_ram, bool wide, bool sel) {
    using namespace maya;
    TableRow row;
    row.style = Style{}.with_fg(pal::label);
    if (sel) row.style = Style{}.with_bg(mix(pal::bg_panel, pal::proc_ac, 0.30)).with_fg(pal::text);

    const double cpu_share = share_of(u.cpu, total_cpu);
    const Color cpu_c = load_color(cpu_share);
    const Color mem_c = u.mem_share > 0.5 ? pal::hot
                      : u.mem_share > 0.25 ? pal::warn : pal::mem_ac;

    row.cells.emplace_back(sel ? "\xe2\x96\x8d" : "");   // ▍ cursor rail
    // root gets its own ink: "root is at 300%" is a different sentence from
    // "a user is at 300%", and on a shared box that distinction matters.
    row.cells.push_back(TableCell{}.span(u.user,
        Style{}.with_bold().with_fg(u.user == "root" ? pal::crit
                                  : u.user == "?"   ? pal::dim : pal::text)));
    // CPU as a share of the WHOLE MACHINE, matching the bar beside it. The
    // raw htop-style sum ("1179%") is unreadable without knowing the core
    // count, and printing it next to a share-scaled bar made the number and
    // the bar disagree — two different quantities in one column.
    row.cells.push_back(TableCell{}.span(fmt::pct1(cpu_share),
        Style{}.with_bold().with_fg(cpu_c)));
    if (wide)
        row.cells.push_back(TableCell::dyn([cpu_share, cpu_c](int w) -> TableCell {
            return TableCell{}.span(bar_str(cpu_share, w), Style{}.with_fg(cpu_c));
        }));
    row.cells.push_back(TableCell{}.span(humanize_bytes(Bytes{u.rss}),
        Style{}.with_bold().with_fg(mem_c)));
    if (wide)
        row.cells.push_back(TableCell::dyn([share = u.mem_share, mem_c](int w) -> TableCell {
            return TableCell{}.span(bar_str(share, w), Style{}.with_fg(mem_c));
        }));
    // A zombie count rides the PROCS cell — an admin scanning this pane wants
    // "12 (3Z)" to jump out; that's someone's orphaned mess to clean up.
    {
        std::string p = std::to_string(u.procs);
        if (u.zombies > 0) p += " (" + std::to_string(u.zombies) + "Z)";
        row.cells.push_back(TableCell{}.span(p,
            Style{}.with_fg(u.zombies > 0 ? pal::hot : pal::label)));
    }
    row.cells.emplace_back(std::to_string(u.threads));
    row.cells.push_back(TableCell{}.span(
        u.io > 1024 ? std::string(humanize_rate(ByteRate{u.io})) : std::string("\xc2\xb7"),
        Style{}.with_fg(u.io > 1024 * 1024 ? pal::hot : pal::dim)));
    row.cells.push_back(TableCell::dyn(
        [name = u.top_name, pid = u.top_pid](int w) -> TableCell {
            if (pid <= 0) return {"\xe2\x80\x94"};
            const std::string tail = " (" + std::to_string(pid) + ")";
            const int room = std::max(2, w - static_cast<int>(tail.size()));
            return {maya::truncate_end(name, room) + tail};
        }));
    return row;
}

inline maya::Table users_table(const std::vector<UserStat>& us, double total_cpu,
                               std::uint64_t total_ram, bool wide, int sel) {
    using namespace maya;
    Table tbl(user_columns(wide));
    auto& cfg = tbl.config();
    cfg.cell_padding   = 0;
    cfg.column_gap     = 1;
    cfg.show_separator = false;
    cfg.header_style   = Style{}.with_bold().with_fg(mix(pal::proc_ac, pal::text, 0.15));
    cfg.header_bg      = mix(pal::bg_panel, pal::proc_ac, 0.14);
    cfg.stripe_rows    = true;
    cfg.alt_row_style  = Style{}.with_bg(mix(pal::bg_panel, pal::proc_ac, 0.06));
    cfg.scrollbar_thumb_color = pal::proc_ac;
    cfg.scrollbar_track_color = pal::faint;

    std::vector<TableRow> rows;
    rows.reserve(us.size());
    for (int i = 0; i < static_cast<int>(us.size()); ++i)
        rows.push_back(user_row(us[static_cast<std::size_t>(i)], total_cpu,
                                total_ram, wide, i == sel));
    tbl.set_rows(std::move(rows));
    return tbl;
}

// Scroll ceiling for the users pane, so ↑↓ can't run past the last row.
inline int users_scroll_max(const Snapshot& s, const Ctx& cx) {
    const int n = static_cast<int>(user_stats(s.procs, s.mem.total.value).size());
    const int view = std::max(1, cx.body_h - 8);   // headline block + rules
    return std::max(0, n - view);
}

// ── the pane body ─────────────────────────────────────────────────────────
inline std::vector<Element> users_body(const Snapshot& s, const Ctx& cx,
                                       UserSort sort, int sel) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> b;

    const std::vector<UserStat> us = user_stats(s.procs, s.mem.total.value, sort);
    if (us.empty()) {
        b.push_back(verdict("no processes to attribute \xe2\x80\x94 nothing to show", pal::dim));
        return b;
    }

    // Machine totals, so each user's share is meaningful rather than a bare
    // percentage of an unstated whole.
    const double total_cpu = std::max(1.0, static_cast<double>(s.cpu.cores.size()) * 100.0);
    double sum_cpu = 0;
    std::uint64_t sum_rss = 0;
    int sum_procs = 0, sum_zombies = 0;
    for (const UserStat& u : us) {
        sum_cpu += u.cpu; sum_rss += u.rss;
        sum_procs += u.procs; sum_zombies += u.zombies;
    }

    b.push_back(section("WHO IS ON THIS BOX", pal::proc_ac,
                        std::to_string(us.size()) + " users \xc2\xb7 "
                        + std::to_string(sum_procs) + " procs"));
    b.push_back(kv3(
        "users", std::to_string(us.size()), pal::proc_ac,
        "processes", std::to_string(sum_procs), pal::label,
        "zombies", std::to_string(sum_zombies), sum_zombies ? pal::hot : pal::good));
    b.push_back(kv3(
        "busiest", us.empty() ? "—" : us[0].user, pal::cpu_ac,
        "cpu used", fmt::pct1(sum_cpu / total_cpu), load_color(share_of(sum_cpu, total_cpu)),
        "ram used", humanize_bytes(Bytes{sum_rss}), pal::mem_ac));

    // Read the room, the way every other pane does: name the heaviest user and
    // say whether they're actually a problem, rather than making the admin do
    // the comparison themselves.
    {
        const UserStat& top = us[0];
        const double top_share = share_of(top.cpu, total_cpu);
        const double top_mem = top.mem_share;
        std::string msg; maya::Color vc;
        if (sum_zombies > 20) {
            msg = "\xe2\x96\xb2 " + std::to_string(sum_zombies) +
                  " zombie processes \xe2\x80\x94 a parent somewhere is not reaping its children";
            vc = pal::hot;
        } else if (top_share > 0.60) {
            msg = "\xe2\x96\xb2 " + top.user + " is using " + fmt::pct1(top_share) +
                  " of this machine's CPU across " + std::to_string(top.procs) +
                  " processes \xe2\x80\x94 that's the load";
            vc = pal::crit;
        } else if (top_mem > 0.50) {
            msg = "\xe2\x96\xb2 " + top.user + " holds " + humanize_bytes(Bytes{top.rss}) +
                  " of RAM (" + fmt::pct1(top_mem) + " of the box)";
            vc = pal::hot;
        } else if (us.size() == 1) {
            msg = "\xe2\x97\x8f single-tenant \xe2\x80\x94 everything here belongs to " + top.user;
            vc = pal::good;
        } else {
            msg = "\xe2\x97\x8f load is spread across " + std::to_string(us.size()) +
                  " users; " + top.user + " is the busiest at " + fmt::pct1(top_share);
            vc = pal::good;
        }
        b.push_back(verdict(msg, vc));
    }
    b.push_back(gap_row());

    // The table. Sorted-column name goes in the chip so the ordering is never
    // a mystery, and the hint spells out the two destructive gestures.
    const char* sort_name = sort == UserSort::Cpu ? "by cpu"
                          : sort == UserSort::Mem ? "by mem"
                          : sort == UserSort::Procs ? "by procs"
                          : sort == UserSort::Io ? "by i/o" : "by name";
    b.push_back(section("USERS", pal::proc_ac, sort_name));

    const int view = std::max(1, cx.body_h - static_cast<int>(b.size()) - 2);
    Table tbl = users_table(us, total_cpu, s.mem.total.value, cx.wide, sel);
    tbl.config().visible_rows = view;
    tbl.config().window_top   = cx.scroll;
    b.push_back(tbl.build());

    return b;
}

}  // namespace rockbottom::ui::detail
