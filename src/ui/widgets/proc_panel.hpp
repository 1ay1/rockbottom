// widgets/proc_panel.hpp — interactive process table.
//
// The table itself is maya::Table — column plan (solve_columns), header
// ink tiers + sort arrow, cursor bar + selected-row strip, sticky host
// scroll window, scrollbar, row/header hit rects, … truncation: ALL of
// it is the framework's. This file supplies only what is rockbottom's:
//   • the column set and its shed order (PORT → DISK → meter → THR → MEM%)
//   • rich cells: the flow tree (weight gutter + heat rails + chevron),
//     the dim command trail, the inline CPU meter, state dots
//   • the culprit row (» edge marker in the health color)
//   • a kill-confirm strip that replaces the header while pending
//   • the active filter / sort / follow chip on the panel border

#pragma once

#include <maya/maya.hpp>
#include <maya/widget/table.hpp>

#include "../../core/metrics.hpp"
#include "../state.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"
#include "hit_ids.hpp"
#include "meter.hpp"
#include "panel.hpp"

#include <algorithm>
#include <csignal>
#include <string>
#include <utility>
#include <vector>

namespace rockbottom::ui {

class ProcPanel {
    const Snapshot& snap_;
    const ProcView& view_;

    // Column order. The meter is its own column so it can breathe
    // 8 → 14 cells (weight 1) while NAME takes the rest of the slack
    // (weight 3) — same plan the old hand-rolled solver used.
    enum Col : int {
        CPid, CUser, CName, CPort, CMeter, CCpu, CMem, CMemP, CIo, CState,
        CThr
    };

public:
    ProcPanel(const Snapshot& s, const ProcView& v) : snap_(s), view_(v) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        const auto& procs = view_.procs;
        const int n = static_cast<int>(procs.size());

        // Culprit detection on the visible (filtered) list.
        int hi = -1;
        double best = -1;
        for (int i = 0; i < n; ++i) {
            double v = view_.sort == SortKey::Mem
                           ? static_cast<double>(procs[static_cast<std::size_t>(i)]->rss.value)
                           : procs[static_cast<std::size_t>(i)]->cpu;
            if (v > best) { best = v; hi = i; }
        }
        const bool loud = (view_.sort == SortKey::Mem)
            ? (hi >= 0 && procs[static_cast<std::size_t>(hi)]->mem_share.percent() > 8)
            : (hi >= 0 && best > 25);

        Table tbl(columns(), config());
        std::vector<TableRow> rows;
        rows.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            rows.push_back(proc_row(*procs[static_cast<std::size_t>(i)],
                                    i == view_.selected,
                                    loud && i == hi, i));
        tbl.set_rows(std::move(rows));
        tbl.set_selected(view_.selected);

        // Kill confirmation strip replaces the header while pending
        // (config() already dropped the header row, so the row budget
        // is unchanged).
        // The table draws its own ╭ PROCESSES ─╮ frame now (show_border),
        // so we return it directly instead of wrapping in an app Panel —
        // otherwise the box doubles. The sort/filter chip rides in the
        // title so the border rail still carries view state. ONE grow only:
        // the table's own build carries the grow so it fills its slot in the
        // outer vstack without a second nested grow compounding (which
        // over-subscribed the band and starved the top stat panels).
        // RGB themes own a dark canvas: the maya Table draws its ╭╮ frame with
        // no bg of its own, so wrap it in a box tinted with the theme canvas —
        // the ambient box bg carries under the border line + padding instead of
        // leaking the terminal default. Native defers (no wrap).
        auto themed = [](Element e) -> Element {
            return theme_paints_canvas()
                ? (Element{std::move(e)} | grow(1) | bgc(theme_canvas())).build()
                : (Element{std::move(e)} | grow(1)).build();
        };
        if (view_.pending) {
            return themed(v(confirm_strip(), (tbl.build() | grow(1)).build()).build());
        }
        return themed(tbl.build());
    }

private:
    // ── The column set ──
    // Shed order as the panel narrows (lowest keep first): PORT → DISK →
    // meter → THR → MEM%; everything else never sheds. Header ink is a
    // three-tier hierarchy ON THE RAIL BAND (bright_black): the active sort
    // column is the only loud thing (sort_header_style: accent + bold + ▾),
    // sortable columns are bright white (they read as clickable), inert ones
    // sit a tier lower in normal white. dim/faint ink is BANNED here — it's
    // the same ANSI slot as the band, i.e. invisible.
    [[nodiscard]] std::vector<maya::ColumnDef> columns() const {
        using namespace maya;
        const Style sortable = Style{}.with_bold().with_fg(pal::white);
        const Style inert    = Style{}.with_fg(pal::text);
        // MEM% shares SortKey::Mem with MEM. Only MEM (the sort_col) gets
        // the arrow; MEM% echoes the accent so the pair reads as one key.
        const Style memp_st = view_.sort == SortKey::Mem
            ? Style{}.with_bold().with_fg(pal::proc_ac) : sortable;
        return {
            {.header = "PID",  .align = ColumnAlign::Right, .keep = kKeepAlways,
             .header_style = sortable, .hit_index = static_cast<int>(SortKey::Pid)},
            {.header = "USER", .keep = kKeepAlways,
             .header_style = inert, .hit_index = kNoHeaderHit},
            {.header = "NAME", .keep = kKeepAlways, .weight = 3.0f,
             // In FLOW (tree) mode the NAME cell also carries the tree
             // furniture (gutter + rail prefix + chevron), which can eat most
             // of a weight-3 share on a narrow terminal, crushing the actual
             // process name to "sys…". Raise the floor in tree mode so the
             // column solver SHEDS the low-priority columns (PORT, THR, MEM%,
             // DISK) before starving the name.
             .min_width = view_.tree ? 20 : 8,
             .header_style = sortable, .hit_index = static_cast<int>(SortKey::Name)},
            {.header = "PORT", .align = ColumnAlign::Right, .keep = 1,
             .header_style = sortable, .hit_index = static_cast<int>(SortKey::Port)},
            {.header = "",     .keep = 3, .weight = 1.0f, .min_width = 8,
             .max_width = 14, .hit_index = kNoHeaderHit},   // inline CPU meter
            {.header = "CPU",  .align = ColumnAlign::Right, .keep = kKeepAlways,
             .header_style = sortable, .hit_index = static_cast<int>(SortKey::Cpu)},
            {.header = "MEM",  .align = ColumnAlign::Right, .keep = kKeepAlways,
             .header_style = sortable, .hit_index = static_cast<int>(SortKey::Mem)},
            {.header = "MEM%", .align = ColumnAlign::Right, .keep = 5,
             .header_style = memp_st, .hit_index = static_cast<int>(SortKey::Mem)},
            {.header = "DISK", .align = ColumnAlign::Right, .keep = 2,
             .header_style = sortable, .hit_index = static_cast<int>(SortKey::Io)},
            {.header = "S",    .align = ColumnAlign::Center, .keep = kKeepAlways,
             .header_style = inert, .hit_index = kNoHeaderHit},
            {.header = "THR",  .align = ColumnAlign::Right, .keep = 4,
             .header_style = inert, .hit_index = kNoHeaderHit},
        };
    }

    [[nodiscard]] maya::TableConfig config() const {
        using namespace maya;
        TableConfig cfg;
        cfg.selectable   = true;
        cfg.stripe_rows  = false;
        cfg.cell_padding = 0;
        cfg.column_gap   = 1;
        cfg.show_separator = true;                // ── rule under the header
        cfg.show_border  = true;                  // ╭ PROCESSES ─╮ frame
        cfg.title        = title_with_chip();
        cfg.border_color = pal::border;
        // RGB themes own a dark canvas; the maya Table draws its ╭╮ frame
        // with no bg of its own, so hand it the theme canvas — the frame
        // glyphs + padding read on the theme bg instead of leaking the
        // terminal default. Native (index 0) leaves it unset (transparent).
        if (theme_paints_canvas()) cfg.box_bg = theme_canvas();
        cfg.show_header  = view_.pending == nullptr;  // strip replaces it
        cfg.header_bg    = pal::rail;             // filled header pill band
        cfg.header_style = Style{}.with_fg(pal::text);  // band-safe fallback ink
        cfg.sort_header_style = Style{}.with_bold().with_fg(pal::proc_ac);
        cfg.sort_col     = sort_column();
        cfg.sort_desc    = view_.sort_desc;
        // Row ink is baked per-cell below (lift toward white on the
        // cursor row); the table adds only the bar + the strip.
        cfg.selected_style   = Style{};
        cfg.selected_bg      = mix(pal::sel_bg, pal::proc_ac, 0.10);
        cfg.cursor_bar_color = pal::proc_ac;
        cfg.window_top   = view_.scroll;          // sticky window: model-owned
        cfg.visible_rows = view_.max_rows;        // window to the slot the layout
                                                  // gave us — without this the table
                                                  // reports its full N-row natural
                                                  // height as its flex basis and
                                                  // over-subscribes the outer vstack,
                                                  // shrinking the top stat band.
        cfg.scrollbar_thumb_color = pal::proc_ac;
        cfg.scrollbar_track_color = pal::faint;
        cfg.row_hit_kind    = HK_ProcRow;
        cfg.header_hit_kind = HK_SortCol;         // hit_index = SortKey
        cfg.empty_text = view_.filter.empty()
            ? "no processes"
            : "nothing matches \"" + view_.filter + "\"";
        return cfg;
    }

    [[nodiscard]] int sort_column() const {
        switch (view_.sort) {
            case SortKey::Cpu:  return CCpu;
            case SortKey::Mem:  return CMem;
            case SortKey::Io:   return CIo;
            case SortKey::Pid:  return CPid;
            case SortKey::Name: return CName;
            case SortKey::Port: return CPort;
        }
        return CCpu;
    }

    [[nodiscard]] std::string title_with_chip() const {
        const int n = static_cast<int>(view_.procs.size());
        const char* arrow = view_.sort_desc ? "▼" : "▲";
        std::string mode = view_.tree ? "flow" : ("sort " + sort_label() + arrow);
        std::string chip = view_.filtering ? "/" + view_.filter + "▌"
                         : !view_.filter.empty() ? "/" + view_.filter + " · " + mode
                         : mode + " · " + std::to_string(n);
        if (view_.follow_pid)
            chip = "◉ follow " + std::to_string(view_.follow_pid) + " · " + chip;
        return "PROCESSES · " + chip;
    }

    [[nodiscard]] std::string sort_label() const {
        switch (view_.sort) {
            case SortKey::Cpu:  return "cpu";
            case SortKey::Mem:  return "mem";
            case SortKey::Io:   return "i/o";
            case SortKey::Pid:  return "pid";
            case SortKey::Name: return "name";
            case SortKey::Port: return "port";
        }
        return "cpu";
    }

    [[nodiscard]] maya::Element confirm_strip() const {
        using namespace maya;
        using namespace maya::dsl;
        const auto& p = *view_.pending;
        const bool hard = p.sig == SIGKILL;
        const bool group = p.pids.size() > 1;
        // Non-lethal signals (STOP/CONT/HUP/USR…) shouldn't wear the crit/warn
        // "kill" tint or verb — only TERM/KILL are destructive-by-intent.
        const bool lethal = p.sig == SIGKILL || p.sig == SIGTERM ||
                            p.sig == SIGQUIT || p.sig == SIGABRT || p.sig == SIGINT;
        Color c = hard ? pal::crit : lethal ? pal::warn : pal::sky;
        std::string what = group
            ? "ALL " + std::to_string(p.pids.size()) + " × " + p.name
            : p.name + " (" + std::to_string(p.pid) + ")";
        // Verb reads naturally per signal; the exotic ones say "send SIGX to".
        std::string verb =
              p.sig == SIGKILL ? "force-kill "
            : p.sig == SIGTERM ? "end "
            : (p.sig == SIGSTOP || p.sig == SIGTSTP) ? "suspend "
            : p.sig == SIGCONT ? "resume "
            : "send " + sig_name(p.sig) + " to ";
        std::string q = verb + what + "?";
        return (h(
            text(" " + q + " ") | nowrap | Bold | fgc(pal::bg) | bgc(c),
            text("  y") | nowrap | Bold | fgc(pal::good),
            text("·confirm ") | nowrap | fgc(pal::dim),
            text("n") | nowrap | Bold | fgc(pal::crit),
            text("·cancel") | nowrap | fgc(pal::dim)
        )).build();
    }

    [[nodiscard]] maya::TableRow proc_row(const ProcInfo& p, bool selected,
                                          bool culprit, int idx) const {
        using namespace maya;

        // Tree rollup / context lookups (index-aligned to the ordered view).
        // A COLLAPSED parent surfaces its whole subtree's CPU%/RSS instead of
        // just its own, so a folded node still shows what it's hiding (btop
        // idiom). A CONTEXT row (kept only as an ancestor of a filter match)
        // is dimmed so the matched leaf's lineage reads as scaffolding.
        const bool row_collapsed = view_.tree && idx >= 0
            && idx < static_cast<int>(view_.collapsed_row.size())
            && view_.collapsed_row[static_cast<std::size_t>(idx)];
        const bool row_context = view_.tree && idx >= 0
            && idx < static_cast<int>(view_.context_row.size())
            && view_.context_row[static_cast<std::size_t>(idx)];
        double disp_cpu = p.cpu;
        double disp_rss = static_cast<double>(p.rss.value);
        if (row_collapsed) {
            if (idx < static_cast<int>(view_.sub_cpu.size()))
                disp_cpu = view_.sub_cpu[static_cast<std::size_t>(idx)];
            if (idx < static_cast<int>(view_.sub_mem.size()))
                disp_rss = view_.sub_mem[static_cast<std::size_t>(idx)];
        }
        const double disp_cpu_frac = std::clamp(disp_cpu / 100.0, 0.0, 1.0);

        // The cursor row rides the table's bg strip + accent edge bar (the
        // modern list-selection idiom). Ink lifts to the BRIGHT ANSI slot so
        // hues stay meaningful but brighter; bold is reserved for the name
        // and PID (the row's identity) — the strip already does the shouting.
        auto lift = [&](Color c) { return selected ? brighten(c) : c; };
        auto cell_st = [&](Color c) { return Style{}.with_fg(lift(c)); };
        const Color quiet  = selected ? pal::text  : pal::dim;    // dim ink, lifted
        const Color hushed = selected ? pal::label : pal::faint;  // faint ink, lifted

        Style name_st = Style{}.with_fg(culprit ? pal::crit : selected ? pal::white : pal::text);
        if (culprit || selected) name_st = name_st.with_bold();
        // Context rows (kept only to keep a matched leaf's lineage) recede: no
        // bold, muted ink — they're scaffolding, not results.
        if (row_context && !selected && !culprit)
            name_st = Style{}.with_fg(mix(pal::dim, pal::bg_panel, 0.15));
        Style cpu_st = cell_st(cpu_color(disp_cpu));
        if (disp_cpu > 50) cpu_st = cpu_st.with_bold();
        const bool cpu_zero = disp_cpu < 0.05;
        if (cpu_zero) cpu_st = cell_st(hushed);

        const char* dot = p.state == 'R' ? "●" : p.state == 'D' ? "◆" : "·";
        Color dot_c = lift(p.state == 'R' ? pal::good
                     : p.state == 'D' ? pal::crit : pal::faint);

        // The active sort column's values get brighter ink so the column the
        // table is ranked by reads as the "spine" of the list.
        const SortKey sk = view_.sort;

        // root-owned rows wear the caution yellow — privileged processes pop
        // without shouting; everyone else gets the classic htop teal so the
        // USER column reads as its own colored band, distinct from NAME.
        Color user_c = p.user == "root" ? pal::warn : pal::teal;

        char cpu_txt[16];
        std::snprintf(cpu_txt, sizeof cpu_txt, "%5.1f", disp_cpu);
        char memp_txt[16];
        std::snprintf(memp_txt, sizeof memp_txt, "%4.1f", p.mem_share.percent());
        const double memp = p.mem_share.percent();
        const bool memp_zero = memp < 0.05;
        // Memory ramp: hogs wear escalating heat so the MEM pair carries
        // color even when sorting by CPU. ≥5% warn · ≥10% hot · ≥20% crit.
        const Color mem_heat = memp >= 20 ? pal::crit
                             : memp >= 10 ? pal::hot
                             : memp >= 5  ? pal::warn : pal::text;

        // Ports: ":80" / ":80 +2" — lowest port plus how many more. Sky color
        // makes network-facing processes pop out of the table.
        std::string port_txt;
        if (!p.ports.empty()) {
            port_txt = ":" + std::to_string(p.ports.front());
            if (p.ports.size() > 1)
                port_txt += " +" + std::to_string(p.ports.size() - 1);
        }

        // Combined disk I/O rate; dim when idle, sky when the process is
        // actually touching the platter so a thrasher pops out.
        const double iorate = p.io_read.per_sec + p.io_write.per_sec;
        std::string io_txt = iorate > 512 ? humanize_rate(ByteRate{iorate}) : "·";
        Color io_c = iorate > 512 ? pal::sky : hushed;

        TableRow row;
        row.cells.reserve(CThr + 1);

        // PID: bright white-lifted on the cursor row (row identity, next to
        // the edge bar), crit on the culprit row, text ink when it's the
        // sort spine, dim otherwise.
        Style pid_st = Style{}.with_fg(selected ? pal::white
                                      : sk == SortKey::Pid ? pal::text
                                      : culprit ? pal::crit : pal::dim);
        if (selected) pid_st = pid_st.with_bold();
        row.cells.emplace_back(std::to_string(p.pid), pid_st);

        row.cells.emplace_back(fmt::clip(p.user, 7), cell_st(user_c));
        row.cells.push_back(name_cell(p, selected, name_st, idx));
        row.cells.emplace_back(std::move(port_txt),
                               sk == SortKey::Port ? cell_st(pal::sky).with_bold()
                                                   : cell_st(pal::sky));
        row.cells.push_back(Meter{disp_cpu_frac}.groove(false).table_cell());
        row.cells.emplace_back(cpu_txt, cpu_st);
        row.cells.emplace_back(humanize_bytes(static_cast<std::uint64_t>(disp_rss)),
                               sk == SortKey::Mem
                                   ? cell_st(memp >= 5 ? mem_heat : pal::white).with_bold()
                                   : cell_st(mem_heat));
        row.cells.emplace_back(memp_txt,
                               cell_st(memp >= 5 ? mem_heat
                                       : memp_zero ? hushed : quiet));
        row.cells.emplace_back(std::move(io_txt),
                               sk == SortKey::Io && iorate > 512
                                   ? cell_st(pal::sky).with_bold()
                                   : cell_st(io_c));
        row.cells.emplace_back(dot, Style{}.with_fg(dot_c));
        row.cells.emplace_back(std::to_string(p.threads), cell_st(quiet));

        // Culprit marker rides the gutter whenever the cursor bar isn't
        // on this row; its red name keeps it identifiable while selected.
        if (culprit) {
            row.edge = "»";
            row.edge_color = pal::crit;
        }
        return row;
    }

    // Per-process CPU ramp. load_color() is calibrated for SYSTEM load
    // (green until 55%); for a single process 55% is already heavy, so the
    // table uses tighter stops — a hog turns warm long before it pegs a core.
    [[nodiscard]] static maya::Color cpu_color(double pct) {
        if (pct < 25) return pal::good;
        if (pct < 60) return pal::warn;
        if (pct < 85) return pal::hot;
        return pal::crit;
    }

    // The NAME cell: tree furniture + name + collapse badge + a dim command
    // trail. Returned as a DYNAMIC cell so it can reason about the column's
    // SOLVED width: the tree furniture and the process name are the payload
    // and are placed first; the command trail is decoration that only appears
    // when GENUINE slack remains after them. On a narrow terminal the old
    // code appended the trail unconditionally and let the table's … truncation
    // fall wherever it landed — which was mid-NAME, shearing "systemd" down to
    // "syste…" and "fish" to "fis…". Now the name wins the budget: the trail
    // is dropped the instant it would cost a single cell of the name, so even
    // at 50 cols every process reads its full (or gracefully …-clipped) NAME.
    [[nodiscard]] maya::TableCell name_cell(const ProcInfo& p, bool selected,
                                            maya::Style name_st, int idx) const {
        using namespace maya;
        // Same bright-slot lift as the data cells — mix() can't tint on the
        // native palette, and dim ink (bright_black) VANISHES on the
        // bright_black selection strip, so brighten() lifts it to white.
        auto lift = [&](Color c) { return selected ? brighten(c) : c; };

        // ── assemble the FIXED furniture (gutter + chevron) and the ELASTIC
        // rail prefix separately. The gutter and chevron are irreducible
        // (1 + 2 cells); the rail prefix ("│  ├─ │  ") grows with tree depth
        // and is the only part that can crowd the name off the row — so it's
        // held apart and elided from the LEFT when the column is tight.
        std::string furniture;              // gutter + trailing chevron
        std::vector<StyledRun> fruns;
        std::string prefix;                 // the elastic rail run
        Color prefix_c = lift(pal::dim);
        // The rail run is emitted between the gutter and the chevron, so we
        // record where in `furniture` the chevron begins to stitch them back
        // in order inside the builder.
        std::size_t chevron_at = 0;
        auto push = [&](const std::string& t, Style st) {
            if (t.empty()) return;
            fruns.push_back({furniture.size(), t.size(), st});
            furniture += t;
        };

        // ══ THE FLOW TREE ═══════════════════════════════════════════
        // The hierarchy's connective tissue CARRIES DATA: a weight gutter
        // sized by sibling CPU share, heat-graded rails tinted by subtree
        // CPU, and a fold chevron. (Full rationale unchanged from before.)
        const bool tree_row = view_.tree && idx >= 0 &&
                              idx < static_cast<int>(view_.tree_prefix.size());
        bool folded = false;
        if (tree_row) {
            const std::size_t I = static_cast<std::size_t>(idx);
            const bool kids = idx < static_cast<int>(view_.has_kids.size())
                              && view_.has_kids[I];
            folded = idx < static_cast<int>(view_.collapsed_row.size())
                     && view_.collapsed_row[I];
            const double share = idx < static_cast<int>(view_.sib_share.size())
                                 ? view_.sib_share[I] : 0.0;
            const double scpu = idx < static_cast<int>(view_.sub_cpu.size())
                                ? view_.sub_cpu[I] : 0.0;
            const bool warm = scpu >= 0.5;
            Color heat = warm ? lift(cpu_color(scpu)) : lift(pal::dim);
            prefix_c = heat;

            static const char* kW[] = {"▁","▂","▃","▄","▅","▆","▇","█"};
            int wl = std::clamp(static_cast<int>(share * 7.999), 0, 7);
            Color gut_c = warm ? heat : lift(pal::dim);
            push(kW[wl], warm && scpu > 40 ? Style{}.with_fg(gut_c).with_bold()
                                           : Style{}.with_fg(gut_c));
            push(" ", Style{});
            prefix = view_.tree_prefix[I];
            chevron_at = furniture.size();       // chevron follows the prefix
            if (kids)
                push(folded ? "▸ " : "▾ ",
                     warm ? Style{}.with_fg(heat).with_bold()
                          : Style{}.with_fg(heat));
        }

        // Collapsed-subtree count badge — part of the NAME payload ("what is
        // this fold hiding"), placed right after the name, before any trail.
        std::string badge;
        if (tree_row && folded
            && idx < static_cast<int>(view_.hidden_count.size())) {
            badge = "  +" + std::to_string(view_.hidden_count[
                                static_cast<std::size_t>(idx)]);
        }

        const std::string name = std::string(fmt::clip(p.name, 64));
        const std::string trail = (!p.cmd.empty() && p.cmd != p.name)
            ? "  " + p.cmd : std::string{};
        const Color badge_c = lift(pal::dim);
        const Color trail_c = selected ? pal::text : pal::dim;

        // Build AT the solved width. Priority order: the NAME must always be
        // readable, so the elastic rail prefix yields to it. Steps:
        //   1. gutter + chevron are fixed (≤3 cells) — always drawn.
        //   2. the rail prefix gets whatever's left after reserving a minimum
        //      name budget; if it doesn't fit, it's elided from the LEFT
        //      (drop outermost ancestors, keep the levels nearest the name)
        //      with a leading … so depth is still signalled.
        //   3. name fills the remainder; badge then trail only on real slack.
        return TableCell::dyn(
            [furniture, fruns, chevron_at, prefix, prefix_c, name, name_st,
             badge, badge_c, trail, trail_c](int w) -> TableCell {
                TableCell c;
                auto span = [&](const std::string& t, Style st) {
                    if (!t.empty()) c.span(t, st);
                };

                // Split the fixed furniture around the chevron insertion
                // point so the (possibly elided) prefix goes in between.
                auto emit_furniture = [&](std::size_t lo, std::size_t hi) {
                    for (const auto& r : fruns) {
                        const std::size_t s = std::max(lo, r.byte_offset);
                        const std::size_t e = std::min(hi, r.byte_offset + r.byte_length);
                        if (s < e)
                            c.span(furniture.substr(s, e - s), r.style);
                    }
                };

                const int gut_w = static_cast<int>(string_width(
                                      furniture.substr(0, chevron_at)));
                const int chev_w = static_cast<int>(string_width(
                                       furniture.substr(chevron_at)));
                const int nw = static_cast<int>(string_width(name));
                const int bw = static_cast<int>(string_width(badge));
                const int pw = static_cast<int>(string_width(prefix));

                // Name budget: reserve enough for a readable name (up to the
                // name's own width, capped at ~12 so a very long name doesn't
                // erase every rail). The prefix uses whatever survives after
                // the fixed furniture + this reserved budget.
                const int name_budget = std::min(nw, 12);
                const int prefix_room =
                    std::max(0, w - gut_w - chev_w - name_budget);

                emit_furniture(0, chevron_at);
                if (pw <= prefix_room) {
                    span(prefix, Style{}.with_fg(prefix_c));
                } else if (prefix_room >= 2) {
                    // Elide from the LEFT: keep the (prefix_room-1) rightmost
                    // cells (levels nearest the name) behind a leading ….
                    const std::string kept = last_cells(prefix, prefix_room - 1);
                    span("…", Style{}.with_fg(prefix_c));
                    span(kept, Style{}.with_fg(prefix_c));
                }
                // else: no room for any rail — gutter + chevron alone signal it.
                emit_furniture(chevron_at, furniture.size());

                span(name, name_st);
                span(badge, Style{}.with_fg(badge_c));

                const int used = gut_w + chev_w
                    + std::min(pw, prefix_room) + nw + bw;
                if (!trail.empty() && w - used >= 6)
                    span(trail, Style{}.with_fg(trail_c));
                return c;
            });
    }

    // Keep the last `cells` display-columns of a UTF-8 rail prefix (used to
    // elide deep tree furniture from the left). Walks byte-wise from the end
    // over the prefix's fixed-width glyphs (box-drawing + spaces are all
    // single-width), so it's exact for the guide alphabet the tree emits.
    [[nodiscard]] static std::string last_cells(const std::string& s, int cells) {
        if (cells <= 0) return {};
        const int total = static_cast<int>(maya::string_width(s));
        if (cells >= total) return s;
        // Drop leading display columns until `cells` remain. The prefix is
        // built from single-width glyphs, so column count == codepoint count;
        // skip whole UTF-8 codepoints from the front.
        int to_drop = total - cells;
        std::size_t i = 0;
        while (i < s.size() && to_drop > 0) {
            unsigned char ch = static_cast<unsigned char>(s[i]);
            std::size_t adv = ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4;
            i += adv;
            --to_drop;
        }
        return s.substr(i);
    }
};

}  // namespace rockbottom::ui
