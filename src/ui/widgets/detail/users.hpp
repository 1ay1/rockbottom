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
    // DISK outranks I/O and THR: "who is filling the disk" is the question
    // that gets an admin paged, and it's the one column no other tool here
    // answers. It sheds only just before PROCS.
    cols.push_back({.header = "DISK", .keep = 1, .min_width = 7});
    // DISK% next to it, with its own meter. The absolute figure alone can't
    // be ranked — "145G" means nothing until you know whether the filesystem
    // is 200G or 2T — so this is the column you actually sort by.
    cols.push_back({.header = "DISK%", .keep = 2, .min_width = 6});
    if (wide) cols.push_back({.header = "", .keep = 5, .weight = 1.6f, .min_width = 6});
    cols.push_back({.header = "BUSIEST", .keep = 6,
                    .weight = 2.6f, .min_width = 10, .max_width = 28});
    return cols;
}

// Which sort key each COLUMN of user_columns() corresponds to, index-aligned.
// Built right next to the column list so the two cannot drift: a column added
// above without a matching entry here would silently make every header to its
// right sort by the wrong thing. nullopt = not a sortable column (the indent,
// the meters, BUSIEST).
inline std::vector<std::optional<UserSort>> user_column_sorts(bool wide) {
    std::vector<std::optional<UserSort>> s;
    // Reserve up front. This is the exact column count below, so the vector
    // never reallocates — and GCC 14 at -O3 otherwise emits a bogus
    // "writing 1 byte into a region of size 0" -Wstringop-overflow on the
    // grow path, because it can't prove the fresh allocation is big enough
    // for an optional<enum>. Reserving is the honest fix rather than pragma
    // -ing the warning away: it removes the reallocation the warning is
    // about. (Local GCC 16 doesn't emit it; CI's GCC 14 does.)
    s.reserve(wide ? 13 : 10);
    s.push_back(std::nullopt);              // pane indent
    s.push_back(UserSort::Name);            // USER
    s.push_back(UserSort::Cpu);             // CPU%
    if (wide) s.push_back(UserSort::Cpu);   //   cpu meter
    s.push_back(UserSort::Mem);             // MEM
    if (wide) s.push_back(UserSort::Mem);   //   mem meter
    s.push_back(UserSort::Procs);           // PROCS
    s.push_back(UserSort::Procs);           // THR
    s.push_back(UserSort::Io);              // I/O
    s.push_back(UserSort::Disk);            // DISK
    s.push_back(UserSort::Disk);            // DISK%
    if (wide) s.push_back(UserSort::Disk);  //   disk meter
    s.push_back(std::nullopt);              // BUSIEST
    return s;
}

// One table row for a user. `sel` marks the cursor row: an admin is about to
// press a key that signals everything this row owns, so which row is armed has
// to be unmistakable, not a subtle tint.
inline maya::TableRow user_row(const UserStat& u, double total_cpu,
                               std::uint64_t /*total_ram*/, bool wide, bool sel) {
    using namespace maya;
    TableRow row;
    row.style = Style{}.with_fg(pal::label);
    if (sel) row.style = Style{}.with_bg(mix(pal::bg_panel, pal::proc_ac, 0.30)).with_fg(pal::text);

    const double cpu_share = share_of(u.cpu, total_cpu);
    const Color cpu_c = load_color(cpu_share);
    const Color mem_c = u.mem_share > 0.5 ? pal::hot
                      : u.mem_share > 0.25 ? pal::warn : pal::mem_ac;

    row.cells.emplace_back(sel ? "\xe2\x96\x8d" : "");   // ▍ cursor rail
    // USER. root is called out in ink: "root is at 300%" is a different
    // sentence from "a user is at 300%", and on a shared box that distinction
    // matters. The user cell carries a session badge: "ayush ●2" = two live
    // logins. That's the difference between "a daemon account owns processes"
    // and "a person is sitting at this machine right now", which matters a
    // lot before you mass-signal them. A trailing ·svc marks a SYSTEM account
    // (uid below the login threshold): on a normal box the daemons outnumber
    // the humans and without this the three people are buried in forty rows
    // that all look equally like people.
    {
        std::string label = u.user;
        if (u.sessions > 0) label += " \xe2\x97\x8f" + std::to_string(u.sessions);
        else if (u.system)  label += " \xc2\xb7svc";
        row.cells.push_back(TableCell{}.span(label,
            Style{}.with_bold().with_fg(u.user == "root" ? pal::crit
                                      : u.user == "?"   ? pal::dim
                                      : u.system        ? pal::dim
                                      : u.sessions > 0  ? pal::good : pal::text)));
    }
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
    // DISK. The states are genuinely different and must not look alike:
    //   • measured        — "242G"
    //   • against a quota — "48G/50G" — the limit is the whole story. A bare
    //                       red "48G" can't tell you whether the cap is 50G
    //                       or 500G, so the number alone was unactionable.
    //   • still scanning  — "≥112G" (a floor, because the walk was truncated)
    //   • never measured  — "—", NOT "0". Printing a confident zero for a home
    //                       nobody has walked is a lie an admin would act on.
    {
        std::string txt;
        maya::Color dc = pal::dim;
        if (!u.disk_known) {
            txt = "\xe2\x80\x94";
        } else {
            txt = (u.disk_partial ? "\xe2\x89\xa5" : "")
                + std::string(humanize_bytes(Bytes{u.disk_bytes}));
            // Against a quota the SHARE is the story, not the absolute size:
            // 2G is fine under a 100G cap and an emergency under a 2G one.
            if (u.disk_quota) txt += "/" + std::string(humanize_bytes(Bytes{u.disk_quota}));
            // Over quota is the alarm; near it is the warning.
            if (u.disk_quota && u.disk_bytes >= u.disk_quota) dc = pal::crit;
            else if (u.disk_quota && u.disk_bytes > u.disk_quota * 9 / 10) dc = pal::hot;
            else if (u.disk_bytes > 50ull << 30) dc = pal::warn;
            else dc = pal::disk_ac;
            if (u.disk_partial) dc = pal::dim;   // provisional: don't shout yet
        }
        row.cells.push_back(TableCell{}.span(txt, Style{}.with_fg(dc)));
    }
    // DISK% — the rankable form. "of quota" and "of filesystem" are different
    // questions, so the one in play is marked with a trailing q rather than
    // silently mixing two denominators in one column.
    {
        const bool known = u.disk_known && (u.disk_quota || u.disk_share > 0);
        const maya::Color pc = !known ? pal::dim : load_color(u.disk_share);
        row.cells.push_back(TableCell{}.span(
            known ? fmt::pct1(u.disk_share) + (u.disk_share_of_quota ? "q" : "")
                  : "\xe2\x80\x94",
            Style{}.with_bold().with_fg(pc)));
        if (wide)
            row.cells.push_back(TableCell::dyn([known, sh = u.disk_share, pc](int w) -> TableCell {
                if (!known) return {""};
                return TableCell{}.span(bar_str(sh, w), Style{}.with_fg(pc));
            }));
    }
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
                               std::uint64_t total_ram, bool wide, int sel,
                               UserSort sort = UserSort::Cpu, bool desc = true) {
    using namespace maya;
    // Mark the ACTIVE column in its header — ▼ biggest-first, ▲ reversed. The
    // glyph is both the answer to "what am I looking at" and the affordance
    // that says headers are clickable, so it has to track direction or the
    // second click looks like it did nothing.
    std::vector<ColumnDef> cols = user_columns(wide);
    const std::vector<std::optional<UserSort>> keys = user_column_sorts(wide);
    for (std::size_t i = 0; i < cols.size() && i < keys.size(); ++i) {
        if (!keys[i] || *keys[i] != sort) continue;
        if (cols[i].header.empty()) continue;   // meter columns have no label
        cols[i].header += desc ? "\xe2\x96\xbc" : "\xe2\x96\xb2";
        // The glyph needs a cell, or the header clips its own last letter.
        cols[i].min_width += 1;
    }
    Table tbl(std::move(cols));
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
    // Rows register hit_id(HK_UserRow, row_index) so a click selects the user
    // instead of closing the pane. maya resolves the innermost hit first, so
    // this wins over the card-wide HK_DetailBody tag.
    cfg.row_hit_kind   = HK_UserRow;
    // Headers are clickable, indexed by COLUMN (see user_column_sorts).
    cfg.header_hit_kind = HK_UserSortCol;

    std::vector<TableRow> rows;
    rows.reserve(us.size());
    for (int i = 0; i < static_cast<int>(us.size()); ++i)
        rows.push_back(user_row(us[static_cast<std::size_t>(i)], total_cpu,
                                total_ram, wide, i == sel));
    tbl.set_rows(std::move(rows));
    return tbl;
}

// ── the ONE row budget ────────────────────────────────────────────────────
//
// How many table rows the pane paints. This used to be written out three
// times — once where the table is built (correct), once in users_scroll_max
// (body_h-8), once in the app's cursor-follow math (body_h-10) — and the two
// hardcoded guesses disagreed with the truth by up to 2 rows. That gap is not
// cosmetic: it let the selected row scroll out of view, and the selected row
// is what X and K signal. Everything derives from here now.
//
// kUsersChrome counts the fixed rows users_body() stacks ABOVE the table
// (title, two kv3 stat strips, the selected-user identity row, verdict, gap,
// section header) plus the table's own header row.
//
// It is WIDTH-DEPENDENT: kv3 reflows from three pairs on one line to two rows
// below ~78 cols (see its comment), so each of the two strips costs an extra
// row on a narrow terminal. Measured against a real render: 7 rows at 78+,
// 9 below. Hardcoding the wide number would overestimate the viewport on a
// narrow terminal and let the selected row sit under the fold — which is the
// class of bug this constant exists to prevent.
inline constexpr int kUsersChromeWide   = 7;
inline constexpr int kUsersChromeNarrow = 9;

inline int users_view_rows(const Ctx& cx, bool filter_row = false) {
    const int chrome = (cx.w >= 78 ? kUsersChromeWide : kUsersChromeNarrow)
                     + (filter_row ? 1 : 0);
    return std::max(1, cx.body_h - chrome);
}

// Scroll ceiling for the users pane. NOT a separate function any more: the
// pane's scroller derives it from the real measured body, and a second
// hand-rolled estimate here would (a) rebuild the entire roster just to count
// rows and (b) ignore the active filter, so it disagreed with what was drawn
// the moment anyone typed into `/`.

// ── PER-USER DASHBOARD ────────────────────────────────────────────
//
// The roster table answers "who"; this answers "what exactly are they doing".
// It's the whole point of drilling in, and it's laid out the way the other
// hero panes are: a big CPU trace with a real y-axis, a composition bar for
// how their memory sits against the box, then the identity / session / disk /
// exposure blocks, then their heaviest processes both ways.
//
// Everything here reads from the UserStat the rollup already computed — no
// re-walking the process list on the paint path.
inline std::vector<Element> user_dash_body(const UserStat& u, const Snapshot& s,
                                           const Ctx& cx, double total_cpu) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> b;

    const double cpu_share = share_of(u.cpu, total_cpu);
    const maya::Color cpu_c = load_color(cpu_share);
    const maya::Color mem_c = load_color(u.mem_share);

    // ── title ──
    {
        std::string sub;
        if (u.has_account) {
            sub = "uid " + std::to_string(u.uid);
            if (u.system) sub += " \xc2\xb7 system account";
            if (!u.gecos.empty()) sub += " \xc2\xb7 " + u.gecos;
        } else if (u.user == "?") {
            sub = "processes whose owner could not be resolved";
        } else {
            sub = "no passwd entry \xe2\x80\x94 deleted user, or a container uid map";
        }
        b.push_back((h(
            text(" " + u.user) | nowrap | Bold | fgc(pal::white),
            text(u.sessions > 0 ? "  \xe2\x97\x8f" + std::to_string(u.sessions) + " live" : "")
                | nowrap | Bold | fgc(pal::good),
            text("   " + sub) | nowrap | fgc(pal::dim)
        )).build());
    }
    b.push_back(gap_row());

    // The hero: this user's CPU over time, as a share of the WHOLE machine.
    // Peak-normalizing would lie here — the question is "how much of the box
    // is this person", so the axis stays pinned to total capacity. The rings
    // hold 0..1 per process, so their SUM is "cores busy"; dividing by the
    // core count puts it on the same 0..1 rail as every other hero graph.
    //
    // LIFETIME: hero_graph() keeps the `const float*` it is handed and reads
    // it at PAINT time, long after this function has returned — the same trap
    // traffic_hero documents. A local array would dangle, so the scaled ring
    // is captured BY VALUE in a component that outlives the frame, and the
    // pointer only ever points into that captured copy.
    {
        const double ncores = std::max(1.0, total_cpu / 100.0);
        std::array<float, 48> hist{};
        for (std::size_t i = 0; i < hist.size(); ++i)
            hist[i] = static_cast<float>(
                std::clamp(static_cast<double>(u.cpu_history[i]) / ncores, 0.0, 1.0));
        const int n = std::clamp(u.hist_len, 0, 48);
        const int gh = std::max(5, cx.graph_h);
        std::array<float, 48> memline{};
        memline.fill(static_cast<float>(std::clamp(u.mem_share, 0.0, 1.0)));
        b.push_back(Element{maya::ComponentElement{
            .render = [hist, memline, n, gh, cpu_share, cpu_c](int, int) -> Element {
                // RAM rides along as a flat overlay line. A user's memory
                // doesn't have its own ring, but drawing it on the same axis
                // still answers the question the graph raises — "is this a
                // cpu problem or a memory problem" — without a second chart.
                // BOTH arrays are captured BY VALUE: hero_graph keeps the
                // pointers and dereferences them at paint time, so a local
                // would dangle (this bit once already).
                return hero_graph(cpu_share, cpu_c, "cpu",
                                  hist.data(), n, gh, cpu_c,
                                  n > 1 ? memline.data() : nullptr, n, pal::mem_ac);
            },
        }});
        // Name both traces, with their live figures — an unlabelled overlay
        // is just a mystery line.
        b.push_back((h(
            text("  \xe2\x96\x88 ") | nowrap | fgc(cpu_c),
            text("cpu ") | nowrap | fgc(pal::dim),
            text(fmt::pct1(cpu_share)) | nowrap | Bold | fgc(cpu_c),
            text("   \xe2\x94\x80 ") | nowrap | fgc(pal::mem_ac),
            text("ram ") | nowrap | fgc(pal::dim),
            text(fmt::pct1(u.mem_share)) | nowrap | Bold | fgc(pal::mem_ac),
            text("   \xc2\xb7 share of the whole machine") | nowrap | fgc(pal::faint)
        )).build());
        // The ring fills one sample per tick, so a freshly-opened pane has
        // nothing to draw. Say that, rather than showing a flat line at zero
        // that reads as "this user is idle".
        if (n <= 1)
            b.push_back((h(
                text("  collecting history\xe2\x80\xa6 the trace fills one sample per refresh")
                    | nowrap | fgc(pal::faint)
            )).build());
    }
    b.push_back(gap_row());

    // ── how this user's RAM sits inside the machine ──
    // A bare "6.1G" means nothing without the box's size next to it; the
    // composition bar puts them on the same rail.
    {
        const double total = static_cast<double>(s.mem.total.value);
        const double others = std::max(0.0, static_cast<double>(s.mem.used.value) - static_cast<double>(u.rss));
        b.push_back(section("MEMORY", pal::mem_ac,
                            humanize_bytes(Bytes{u.rss}) + " resident"));
        b.push_back(comp_bar({
            {total > 0 ? static_cast<double>(u.rss) / total : 0.0, pal::mem_ac},
            {total > 0 ? others / total : 0.0, mix(pal::mem_ac, pal::bg_panel, 0.62)},
        }));
        b.push_back(comp_legend({
            {u.user, std::string(humanize_bytes(Bytes{u.rss})), pal::mem_ac},
            {"everyone else", std::string(humanize_bytes(Bytes{static_cast<std::uint64_t>(others)})),
             mix(pal::mem_ac, pal::bg_panel, 0.62)},
            {"free", std::string(humanize_bytes(s.mem.available)), pal::faint},
        }));
        b.push_back(kv3(
            "share of ram", fmt::pct1(u.mem_share), mem_c,
            "virtual", std::string(humanize_bytes(Bytes{u.virt})), pal::label,
            "page faults", fmt::count(u.faults_ps) + "/s",
            u.faults_ps > 2000 ? pal::hot : pal::label));
    }
    b.push_back(gap_row());

    // ── workload shape ──
    // Process-state mix is the difference between "busy" and "stuck": a D
    // herd is waiting on I/O and no amount of CPU will help it.
    {
        b.push_back(section("WORKLOAD", pal::cpu_ac,
                            std::to_string(u.procs) + " procs \xc2\xb7 "
                            + std::to_string(u.threads) + " threads"));
        const double np = u.procs > 0 ? static_cast<double>(u.procs) : 1.0;
        b.push_back(comp_bar({
            {u.running  / np, pal::good},
            {u.sleeping / np, mix(pal::cpu_ac, pal::bg_panel, 0.55)},
            {u.dstate   / np, pal::hot},
            {u.stopped  / np, pal::warn},
            {u.zombies  / np, pal::crit},
        }));
        std::vector<LegendItem> legend = {
            {"running", std::to_string(u.running), pal::good},
            {"sleeping", std::to_string(u.sleeping), mix(pal::cpu_ac, pal::bg_panel, 0.55)},
        };
        if (u.dstate)  legend.push_back({"uninterruptible", std::to_string(u.dstate), pal::hot});
        if (u.stopped) legend.push_back({"stopped", std::to_string(u.stopped), pal::warn});
        if (u.zombies) legend.push_back({"zombie", std::to_string(u.zombies), pal::crit});
        b.push_back(comp_legend(std::move(legend)));
        b.push_back(kv3(
            "cpu of box", fmt::pct1(cpu_share), cpu_c,
            "ctx switches", fmt::count(u.csw_ps) + "/s",
            u.csw_ps > 50000 ? pal::hot : pal::label,
            "nice", u.procs == 0 ? "\xe2\x80\x94"
                  : u.nice_min == u.nice_max ? std::to_string(u.nice_min)
                  : std::to_string(u.nice_min) + "\xe2\x80\xa6" + std::to_string(u.nice_max),
            pal::label));
    }
    b.push_back(gap_row());

    // ── I/O, split ── read-heavy and write-heavy are different problems.
    {
        b.push_back(section("DISK I/O", pal::disk_ac,
                            u.io > 1024 ? std::string(humanize_rate(ByteRate{u.io})) : "idle"));
        const double io_scale = std::max(1.0, u.io_read + u.io_write);
        b.push_back(comp_bar({
            {u.io_read  / io_scale, pal::disk_ac},
            {u.io_write / io_scale, pal::amber},
        }));
        b.push_back(comp_legend({
            {"read", std::string(humanize_rate(ByteRate{u.io_read})), pal::disk_ac},
            {"write", std::string(humanize_rate(ByteRate{u.io_write})), pal::amber},
            {"open fds", u.fds > 0 ? std::to_string(u.fds) : "\xe2\x80\x94", pal::label},
        }));
    }
    b.push_back(gap_row());

    // ── storage footprint ── the "who is filling /home" half.
    {
        std::string chip = !u.disk_known ? "not measured"
                         : std::string(u.disk_partial ? "\xe2\x89\xa5" : "")
                           + std::string(humanize_bytes(Bytes{u.disk_bytes}));
        b.push_back(section("STORAGE", pal::disk_ac, chip));
        if (!u.disk_known) {
            // Say WHY it's blank rather than printing a confident zero.
            b.push_back(kv("home usage", u.home.empty()
                           ? "no home directory on record"
                           : "no quota, and no scan has completed yet", pal::dim));
        } else {
            if (u.disk_quota) {
                const double qshare = share_of(static_cast<double>(u.disk_bytes),
                                               static_cast<double>(u.disk_quota));
                b.push_back(bar("of quota", qshare,
                                humanize_bytes(Bytes{u.disk_bytes}) + " / "
                                + humanize_bytes(Bytes{u.disk_quota}),
                                load_color(qshare)));
            } else {
                // No quota: rank against the filesystem the home lives on,
                // when we know its size. "145G" alone can't tell you whether
                // that's a rounding error or most of the disk.
                if (u.disk_share > 0) {
                    b.push_back(bar("of filesystem", u.disk_share,
                                    std::string(u.disk_partial ? "\xe2\x89\xa5" : "")
                                    + std::string(humanize_bytes(Bytes{u.disk_bytes}))
                                    + " of " + std::string(humanize_bytes(
                                          Bytes{static_cast<std::uint64_t>(
                                              static_cast<double>(u.disk_bytes) / u.disk_share)})),
                                    load_color(u.disk_share)));
                } else {
                    b.push_back(kv("home usage",
                                   std::string(u.disk_partial ? "\xe2\x89\xa5" : "")
                                   + std::string(humanize_bytes(Bytes{u.disk_bytes}))
                                   + (u.disk_partial ? "  (walk still running \xe2\x80\x94 this is a floor)" : ""),
                                   u.disk_partial ? pal::dim : pal::disk_ac));
                }
            }
            b.push_back(kv3(
                "source", u.disk_source && *u.disk_source ? u.disk_source : "\xe2\x80\x94",
                pal::label,
                "home", u.home.empty() ? "\xe2\x80\x94" : u.home, pal::label,
                "shell", u.shell.empty() ? "\xe2\x80\x94" : u.shell, pal::label));
        }
    }
    b.push_back(gap_row());

    // ── sessions ── "is a human sitting at this machine right now", and from
    // where. The remote column is the one an admin actually wants.
    if (!u.session_list.empty()) {
        b.push_back(section("LOGIN SESSIONS", pal::good,
                            std::to_string(u.sessions) + " live"));
        for (const LoginSession& ls : u.session_list) {
            std::string where = ls.tty.empty() ? "?" : ls.tty;
            if (!ls.remote.empty()) where += "  from " + ls.remote;
            else                    where += "  local";
            if (!ls.type.empty() && ls.type != "unspecified") where += "  \xc2\xb7 " + ls.type;
            if (ls.leader > 0) where += "  \xc2\xb7 leader " + std::to_string(ls.leader);
            b.push_back((h(
                text("  ") | nowrap,
                text(ls.active ? "\xe2\x97\x8f" : "\xe2\x97\x8b") | nowrap
                    | fgc(ls.active ? pal::good : pal::dim),
                text(" " + where) | nowrap | fgc(pal::text)
            )).build());
        }
        b.push_back(gap_row());
    }

    // ── exposure ── which ports this user has open. On a shared box this is
    // the security question, and it is nowhere else in the UI per-user.
    if (u.port_count > 0) {
        b.push_back(section("LISTENING PORTS", pal::net_ac,
                            std::to_string(u.port_count) + " bound"));
        std::string list;
        const int show = std::min<int>(u.port_count, 24);
        for (int i = 0; i < show; ++i) {
            if (i) list += "  ";
            list += std::to_string(u.ports[static_cast<std::size_t>(i)]);
        }
        if (u.port_count > show) list += "  +" + std::to_string(u.port_count - show) + " more";
        b.push_back(kv("ports", list, pal::net_ac));
        b.push_back(gap_row());
    }

    // ── their heaviest processes, both ways ──
    // CPU and RAM disagree about "biggest" often enough that showing one is
    // misleading; the two lists together are the actual answer.
    if (!u.heaviest_cpu.empty()) {
        b.push_back(section("HEAVIEST BY CPU", pal::cpu_ac, ""));
        const double peak = u.heaviest_cpu.front().cpu;
        int rank = 1;
        for (const UserStat::TopProc& t : u.heaviest_cpu) {
            b.push_back(rank_row(rank++, std::to_string(t.pid), t.name,
                                 peak > 0 ? t.cpu / peak : 0.0, pal::cpu_ac,
                                 fmt::pct1(share_of(t.cpu, total_cpu)), load_color(share_of(t.cpu, total_cpu)), 6,
                                 std::string(humanize_bytes(Bytes{t.rss})), pal::label, 8));
        }
        b.push_back(gap_row());
    }
    if (!u.heaviest_mem.empty()) {
        b.push_back(section("HEAVIEST BY MEMORY", pal::mem_ac, ""));
        const double peak = static_cast<double>(u.heaviest_mem.front().rss);
        int rank = 1;
        for (const UserStat::TopProc& t : u.heaviest_mem) {
            const double sh = s.mem.total.value
                ? static_cast<double>(t.rss) / static_cast<double>(s.mem.total.value) : 0.0;
            b.push_back(rank_row(rank++, std::to_string(t.pid), t.name,
                                 peak > 0 ? static_cast<double>(t.rss) / peak : 0.0, pal::mem_ac,
                                 std::string(humanize_bytes(Bytes{t.rss})), load_color(sh), 8,
                                 fmt::pct1(sh), pal::label, 6));
        }
        b.push_back(gap_row());
    }

    // ── read the room, for this one user ──
    {
        std::string msg; maya::Color vc;
        if (u.zombies > 10) {
            msg = "\xe2\x96\xb2 " + std::to_string(u.zombies) + " zombies \xe2\x80\x94 one of "
                + u.user + "'s parents is not reaping its children";
            vc = pal::hot;
        } else if (u.dstate > 4) {
            msg = "\xe2\x96\xb2 " + std::to_string(u.dstate) + " processes stuck in uninterruptible "
                  "sleep \xe2\x80\x94 they're waiting on storage, not the CPU";
            vc = pal::hot;
        } else if (u.disk_quota && u.disk_known && u.disk_bytes >= u.disk_quota) {
            msg = "\xe2\x96\xb2 over quota \xe2\x80\x94 writes for this user are already failing";
            vc = pal::crit;
        } else if (u.disk_quota && u.disk_known
                   && u.disk_bytes > u.disk_quota * 9 / 10) {
            msg = "\xe2\x96\xb2 within 10% of quota \xe2\x80\x94 worth a word before it bites";
            vc = pal::hot;
        } else if (cpu_share > 0.60) {
            msg = "\xe2\x96\xb2 " + u.user + " is " + fmt::pct1(cpu_share)
                + " of this machine's CPU across " + std::to_string(u.procs) + " processes";
            vc = pal::crit;
        } else if (u.mem_share > 0.50) {
            msg = "\xe2\x96\xb2 " + u.user + " holds " + fmt::pct1(u.mem_share)
                + " of the box's RAM";
            vc = pal::hot;
        } else if (u.procs == 0) {
            msg = u.user + " owns no running processes \xe2\x80\x94 this row exists for the "
                  "account and its disk footprint";
            vc = pal::dim;
        } else {
            msg = u.user + " is not straining this machine";
            vc = pal::good;
        }
        b.push_back(verdict(msg, vc));
    }

    return b;
}

// ── the pane body ─────────────────────────────────────────────────
// `zoom` empty = the roster table. Non-empty = that user's full dashboard.
// Zooming is a VIEW of the same rollup, not a different query, so the two can
// never disagree about a user's numbers.
inline std::vector<Element> users_body(const Snapshot& s, const Ctx& cx,
                                       UserSort sort, int sel,
                                       const std::string& zoom = "",
                                       const std::string& filter = "",
                                       bool filtering = false,
                                       bool desc = true) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> b;

    const std::vector<UserStat> all = user_stats(s.procs, s.mem.total.value, sort,
                                                &s.accounts, &s.sessions,
                                                /*include_idle_accounts=*/true,
                                                home_fs_size(s.disks, s.accounts),
                                                desc);
    // The filter is applied HERE, so the table the app indexes and the table
    // painted are the same object. (users_rows() in the app mirrors this.)
    const std::vector<UserStat> us = filter_users(all, filter);
    if (all.empty()) {
        b.push_back(verdict("no processes to attribute \xe2\x80\x94 nothing to show", pal::dim));
        return b;
    }

    // Machine totals, so each user's share is meaningful rather than a bare
    // percentage of an unstated whole.
    const double total_cpu = std::max(1.0, static_cast<double>(s.cpu.cores.size()) * 100.0);

    // ZOOMED: one user's full dashboard. Resolved by NAME, not by index — the
    // roster re-sorts every tick and an index would drift onto someone else
    // while you were reading. If the user disappears mid-view (logged out,
    // last process exited) we say so rather than silently showing a stranger.
    if (!zoom.empty()) {
        // Searches the UNFILTERED list: a filter narrows the roster, but it
        // shouldn't blank a dashboard you already have open.
        for (const UserStat& u : all)
            if (u.user == zoom)
                return user_dash_body(u, s, cx, total_cpu);
        b.push_back(verdict(zoom + " is gone \xe2\x80\x94 no processes and no account. "
                            "esc goes back to the roster", pal::dim));
        return b;
    }

    double sum_cpu = 0;
    std::uint64_t sum_rss = 0;
    int sum_procs = 0, sum_zombies = 0;
    for (const UserStat& u : us) {
        sum_cpu += u.cpu; sum_rss += u.rss;
        sum_procs += u.procs; sum_zombies += u.zombies;
    }

    b.push_back(section("WHO IS ON THIS BOX", pal::proc_ac,
                        filter.empty()
                            ? std::to_string(us.size()) + " users \xc2\xb7 "
                              + std::to_string(sum_procs) + " procs"
                            : std::to_string(us.size()) + " of "
                              + std::to_string(all.size()) + " users"));
    // The filter line only occupies a row when it's doing something — either
    // you're typing it or it's narrowing the list. A permanently-present
    // empty prompt would cost a roster row for nothing.
    if (filtering || !filter.empty()) {
        b.push_back((h(
            text(" find ") | nowrap | fgc(pal::dim),
            text("/" + filter + (filtering ? "\xe2\x96\x8c" : ""))
                | nowrap | Bold | fgc(pal::sky),
            text(filtering ? "   name \xc2\xb7 uid \xc2\xb7 real name \xc2\xb7 home \xc2\xb7 shell"
                           : "   esc clears")
                | nowrap | fgc(pal::faint)
        )).build());
    }
    // A filter that matches nobody must say so. Falling through would paint
    // an empty table under a "0 of 48 users" header and look like a bug.
    if (us.empty()) {
        b.push_back(gap_row());
        b.push_back(verdict("no user matches \"" + filter + "\" \xe2\x80\x94 esc clears the filter",
                            pal::dim));
        return b;
    }
    b.push_back(kv3(
        "users", std::to_string(us.size()), pal::proc_ac,
        "processes", std::to_string(sum_procs), pal::label,
        "zombies", std::to_string(sum_zombies), sum_zombies ? pal::hot : pal::good));
    b.push_back(kv3(
        "busiest", us.empty() ? "—" : us[0].user, pal::cpu_ac,
        "cpu used", fmt::pct1(sum_cpu / total_cpu), load_color(share_of(sum_cpu, total_cpu)),
        "ram used", humanize_bytes(Bytes{sum_rss}), pal::mem_ac));

    // WHO IS THIS ROW. uid / shell / home / disk provenance were all being
    // collected and joined and then dropped on the floor. They're the fields
    // that answer "is this a person or a daemon, and where does their stuff
    // live" — which is exactly what you want confirmed BEFORE pressing X.
    // disk_source matters just as much: "quota" (exact, kernel-maintained)
    // and a truncated walk that gave up rendered identically apart from a
    // ≥, so a rough floor could read as a hard number.
    if (!us.empty()) {
        const UserStat& sel_u = us[static_cast<std::size_t>(
            std::clamp(sel, 0, static_cast<int>(us.size()) - 1))];
        std::string who = sel_u.user;
        if (sel_u.has_account) {
            who += "  uid " + std::to_string(sel_u.uid);
            if (sel_u.system) who += "  \xc2\xb7 system account";
            if (!sel_u.shell.empty())  who += "  \xc2\xb7 " + sel_u.shell;
            if (!sel_u.home.empty())   who += "  \xc2\xb7 " + sel_u.home;
        } else if (sel_u.user != "?") {
            who += "  \xc2\xb7 no passwd entry (deleted user, or a container uid map)";
        }
        if (sel_u.disk_known && sel_u.disk_source && *sel_u.disk_source)
            who += "  \xc2\xb7 disk via " + std::string(sel_u.disk_source)
                 + (sel_u.disk_partial ? " (still walking)" : "");
        b.push_back(kv("selected", who, pal::proc_ac));
    }

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
                          : sort == UserSort::Io ? "by i/o"
                          : sort == UserSort::Disk ? "by disk" : "by name";
    b.push_back(section("USERS", pal::proc_ac,
                        std::string(sort_name) + (desc ? " \xe2\x96\xbc" : " \xe2\x96\xb2")));

    const int view = users_view_rows(cx, filtering || !filter.empty());
    Table tbl = users_table(us, total_cpu, s.mem.total.value, cx.wide, sel, sort, desc);
    tbl.config().visible_rows = view;
    tbl.config().window_top   = cx.scroll;
    b.push_back(tbl.build());

    return b;
}

}  // namespace rockbottom::ui::detail
