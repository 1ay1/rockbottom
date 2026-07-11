// widgets/proc_panel.hpp — interactive process table.
//
// Renders the filtered/sorted process list with:
//   • a selection bar (▎ edge bar + tinted background) driven by ↑/↓ j/k
//   • the culprit row flagged » in the health color
//   • inline CPU mini-meter per row, state dots
//   • a kill-confirm strip when a signal is pending
//   • the active filter shown in the border chip
//
// The selection window auto-scrolls to keep the cursor visible.

#pragma once

#include <maya/maya.hpp>
#include <maya/widget/scrollbar.hpp>

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
#include <vector>

namespace rockbottom::ui {

class ProcPanel {
    const Snapshot& snap_;
    const ProcView& view_;

public:
    ProcPanel(const Snapshot& s, const ProcView& v) : snap_(s), view_(v) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;

        const auto& procs = view_.procs;
        const int n = static_cast<int>(procs.size());
        // Scroll window that keeps the selection visible.
        const int body_rows = std::max(3, view_.max_rows - 1);
        const bool scrolling = n > body_rows;
        // When the scrollbar shows it eats gutter(1)+bar(1) on the right of the
        // body rows; reserve the same on the header (the header's own gap(1)
        // supplies one col, this blank the other) so THR stays aligned.
        const int rgutter = scrolling ? 1 : 0;

        // Kill confirmation strip replaces the header while pending.
        if (view_.pending) rows.push_back(confirm_strip());
        else               rows.push_back(header_row(rgutter));

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

        int start = 0;
        if (view_.selected >= body_rows) start = view_.selected - body_rows + 1;
        start = std::clamp(start, 0, std::max(0, n - body_rows));

        std::vector<Element> body;
        // NAME cell width, computed analytically from the same tier flags the
        // row uses — a flex cell whose content is wide (cmd trail) bullies the
        // fixed columns out of alignment, so nothing here is left to flex.
        const int row_w = view_.width - (scrolling ? 2 : 0);
        const int name_w = [&] {
            const int w = view_.width;
            const bool show_port = w >= 92;
            const bool show_thr  = w >= 70;
            const bool show_memp = w >= 62;
            const bool show_mem  = w >= 54;
            const bool show_io   = w >= 84;
            int ncols = 6;                       // pid, user, name, meter, cpu, mem
            int fixed = 8 + 8 + (show_mem ? 14 : 8) + 6 + 8 + 2;  // + S dot
            ncols += 1;                          // S
            if (show_port) { fixed += 9; ++ncols; }
            if (show_memp) { fixed += 5; ++ncols; }
            if (show_io)   { fixed += 8; ++ncols; }
            if (show_thr)  { fixed += 4; ++ncols; }
            return std::max(8, row_w - fixed - (ncols - 1));
        }();
        for (int i = start; i < n && i < start + body_rows; ++i)
            body.push_back(proc_row(*procs[static_cast<std::size_t>(i)],
                                    i == view_.selected,
                                    loud && i == hi,
                                    ((i - start) & 1) != 0,
                                    name_w, i));

        if (n == 0)
            body.push_back((text(view_.filter.empty()
                                     ? "no processes"
                                     : "nothing matches \"" + view_.filter + "\"")
                            | fgc(pal::dim)).build());

        // Scrollbar: a self-contained proportional thumb built from plain
        // text segments. We deliberately do NOT use maya::scrollbar_y here —
        // that helper stashes a pointer to the ScrollState you pass it
        // (`bx->scroll_state = &s`) for drag/hover writeback, and our state is
        // a local that dies when build() returns → stack-use-after-return the
        // renderer would later dereference. This bar owns nothing dangling.
        if (scrolling) {
            const int vh = static_cast<int>(body.size());
            const int max_y = std::max(1, n - body_rows);
            const int thumb = std::max(1, vh * vh / std::max(1, n));
            const int track = std::max(0, vh - thumb);
            const int pos = std::clamp(start * track / max_y, 0, track);
            std::vector<Element> barcol;
            for (int r = 0; r < vh; ++r) {
                const bool on = r >= pos && r < pos + thumb;
                barcol.push_back((text(on ? "█" : "│") | nowrap
                                  | fgc(on ? pal::proc_ac : pal::faint)).build());
            }
            rows.push_back((h(
                (v(body) | grow(1)),
                Element{blank()} | width(1),
                v(std::move(barcol)) | width(1)
            )).build());
        } else {
            for (auto& r : body) rows.push_back(std::move(r));
        }

        // Chip: filter beats sort in relevance when active; tree/follow/dir
        // badges make the current view state legible at a glance.
        const char* arrow = view_.sort_desc ? "▼" : "▲";
        std::string mode = view_.tree ? "flow" : ("sort " + sort_label() + arrow);
        std::string chip = view_.filtering ? "/" + view_.filter + "▌"
                         : !view_.filter.empty() ? "/" + view_.filter + " · " + mode
                         : mode + " · " + std::to_string(n);
        if (view_.follow_pid) chip = "◉ follow " + std::to_string(view_.follow_pid) + " · " + chip;

        return Panel("≡", "PROCESSES", pal::proc_ac)
            .chip(chip)
            .grow(1)(std::move(rows));
    }

private:
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

    [[nodiscard]] maya::Element header_row(int rgutter = 0) const {
        using namespace maya;
        using namespace maya::dsl;
        const int w = view_.width;
        // Drop optional columns as width shrinks; NAME is the flex column so
        // it always soaks up the remainder (never overflows, never starves).
        const bool show_port = w >= 92;
        const bool show_thr  = w >= 70;
        const bool show_memp = w >= 62;
        const bool show_mem  = w >= 54;
        const bool show_io   = w >= 84;
        // The header is a quiet RAIL, not a row of shouting labels: no
        // underline wall, a subtle full-width band (bgc on the h-container;
        // maya's ambient-bg inheritance carries it under every label), and a
        // three-tier ink hierarchy — the ACTIVE sort column is the only loud
        // thing (accent + bold + ▾), sortable columns sit in mid ink (they
        // read as clickable), non-sortable ones recede to dim.
        auto hdr = [&](const char* name, SortKey self) {
            const bool on = view_.sort == self;
            const char* ar = view_.sort_desc ? " ▾" : " ▴";
            std::string s = on ? std::string(name) + ar : std::string(name);
            Style st = Style{}.with_fg(on ? pal::proc_ac : pal::label);
            if (on) st = st.with_bold();
            return text(s, st) | nowrap | hit(hit_sort(self));
        };
        auto plain = [&](const char* name) {
            return text(name) | nowrap | fgc(pal::dim);
        };
        // A numeric header sits RIGHT-aligned over the number it labels, so it
        // lines up with the right-aligned values in the rows below (the meter
        // to its left is spanned by a blank).
        auto num_hdr = [&](const char* name, SortKey self, int meter_w, int num_w) {
            return (h(
                blank() | width(meter_w),
                hdr(name, self) | width(num_w) | justify(Justify::End)
            ) | gap(1)).build();
        };
        std::vector<Element> cols;
        cols.push_back((hdr("  PID", SortKey::Pid) | w_<8>).build());
        cols.push_back((plain("USER") | w_<8>).build());
        cols.push_back((hdr("NAME", SortKey::Name) | grow(1)).build());
        if (show_port) cols.push_back((hdr("PORT", SortKey::Port) | w_<9> | justify(Justify::End)).build());
        cols.push_back(num_hdr("CPU", SortKey::Cpu, show_mem ? 14 : 8, 6));
        cols.push_back((hdr("MEM", SortKey::Mem) | w_<8> | justify(Justify::End)).build());
        if (show_memp) cols.push_back((hdr("MEM%", SortKey::Mem) | w_<5> | justify(Justify::End)).build());
        if (show_io) cols.push_back((hdr("DISK", SortKey::Io) | w_<8> | justify(Justify::End)).build());
        cols.push_back((plain("S") | w_<2> | justify(Justify::Center)).build());
        if (show_thr) cols.push_back((plain("THR") | w_<4> | justify(Justify::End)).build());
        if (rgutter > 0) cols.push_back((Element{blank()} | width(rgutter)).build());
        return (h(std::move(cols)) | gap(1) | bgc(pal::track)).build();
    }

    [[nodiscard]] maya::Element proc_row(const ProcInfo& p, bool selected, bool culprit,
                                         bool alt, int name_w, int idx) const {
        using namespace maya;
        using namespace maya::dsl;

        const double cpu_frac = std::clamp(p.cpu / 100.0, 0.0, 1.0);
        const double mem_frac = p.mem_share.v;

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

        // The cursor row rides a bg strip (bgc on the row's h-container;
        // maya's ambient-bg inheritance carries it under every glyph) with a
        // solid accent EDGE BAR on the left — the modern list-selection
        // idiom. Ink lifts toward white so hues stay meaningful but brighter;
        // bold is reserved for the name and PID (the row's identity), not
        // smeared across every cell — the strip already does the shouting.
        auto lift = [&](Color c) { return selected ? mix(c, pal::white, 0.45) : c; };
        auto cell_st = [&](Color c) {
            return Style{}.with_fg(lift(c));
        };
        const Color quiet  = selected ? pal::text  : pal::dim;    // dim ink, lifted
        const Color hushed = selected ? pal::label : pal::faint;  // faint ink, lifted

        Style name_st = Style{}.with_fg(culprit ? pal::crit : selected ? pal::white : pal::text);
        if (culprit || selected) name_st = name_st.with_bold();
        // Context rows (kept only to keep a matched leaf's lineage) recede: no
        // bold, muted ink — they're scaffolding, not results.
        if (row_context && !selected && !culprit)
            name_st = Style{}.with_fg(mix(pal::dim, pal::bg_panel, 0.15));
        Style cpu_st = cell_st(load_color(disp_cpu_frac));
        if (disp_cpu > 50) cpu_st = cpu_st.with_bold();

        const char* dot = p.state == 'R' ? "●" : p.state == 'D' ? "◆" : "·";
        Color dot_c = lift(p.state == 'R' ? pal::good
                     : p.state == 'D' ? pal::crit : pal::faint);

        // The active sort column's values get brighter ink so the column the
        // table is ranked by reads as the "spine" of the list.
        const SortKey sk = view_.sort;

        // root-owned rows get a quiet warm tint on the user column — enough
        // to pick out privileged processes without shouting.
        Color user_c = p.user == "root" ? mix(pal::hot, pal::label, 0.55) : pal::label;

        // Selection edge bar beats culprit marker in the gutter; culprit
        // keeps its red name so it stays identifiable while selected.
        std::string gutter = selected ? "▎" : culprit ? "»" : " ";
        Color gutter_c = selected ? pal::proc_ac : culprit ? pal::crit : pal::dim;

        char cpu_txt[16];
        std::snprintf(cpu_txt, sizeof cpu_txt, "%5.1f", disp_cpu);
        char memp_txt[16];
        std::snprintf(memp_txt, sizeof memp_txt, "%4.1f", p.mem_share.percent());
        // Zero reads as silence: idle figures drop to faint ink so the
        // columns aren't a wall of identical 0.0s and live rows pop.
        const bool cpu_zero  = disp_cpu < 0.05;
        const bool memp_zero = p.mem_share.percent() < 0.05;
        if (cpu_zero) cpu_st = cell_st(hushed);

        // The NAME column owns all the slack; instead of a void, trail the
        // command line in barely-there ink — genuinely useful (which python?
        // whose agentty?) and it fills the table's dead middle.
        std::string cmd_trail;
        if (!p.cmd.empty() && p.cmd != p.name) {
            cmd_trail = p.cmd;
            if (cmd_trail.size() > 96) cmd_trail.resize(96);
        }

        // Ports: ":80" / ":80 +2" — lowest port plus how many more. Sky color
        // makes network-facing processes pop out of the table.
        std::string port_txt;
        if (!p.ports.empty()) {
            port_txt = ":" + std::to_string(p.ports.front());
            if (p.ports.size() > 1)
                port_txt += " +" + std::to_string(p.ports.size() - 1);
        }

        auto row = [&] {
            const int w = view_.width;
            const bool show_port = w >= 92;
            const bool show_thr  = w >= 70;
            const bool show_memp = w >= 62;
            const bool show_mem  = w >= 54;
            const bool show_io   = w >= 84;
            // Combined disk I/O rate; dim when idle, sky when the process is
            // actually touching the platter so a thrasher pops out.
            const double iorate = p.io_read.per_sec + p.io_write.per_sec;
            std::string io_txt = iorate > 512 ? humanize_rate(ByteRate{iorate}) : "·";
            Color io_c = iorate > 512 ? pal::sky : hushed;
            std::vector<Element> cols;
            // Edge bar carries the accent; the PID next to it goes bright
            // white-lifted (row identity) instead of accent-on-accent.
            Style pid_st = Style{}.with_fg(selected ? pal::white
                                          : sk == SortKey::Pid ? pal::text : gutter_c);
            if (selected) pid_st = pid_st.with_bold();
            std::vector<StyledRun> pid_runs;
            std::string pid_txt = gutter + std::to_string(p.pid);
            if (selected) {
                pid_runs.push_back({0, gutter.size(), Style{}.with_fg(gutter_c)});
                pid_runs.push_back({gutter.size(), pid_txt.size() - gutter.size(), pid_st});
                cols.push_back(Element{TextElement{.content = std::move(pid_txt), .style = {},
                                                   .wrap = TextWrap::NoWrap,
                                                   .runs = std::move(pid_runs)}} | width(8));
            } else {
                cols.push_back((text(pid_txt, pid_st) | nowrap | w_<8>).build());
            }
            cols.push_back((text(fmt::clip(p.user, 7), cell_st(user_c)) | nowrap | w_<8>).build());
            {
                // name (styled) + dim command trail, hard-clipped to the
                // analytically computed cell width so fixed columns to the
                // right never shift. In tree mode a dim box-drawing guide and
                // a ▸/▾ collapse chevron ride in front of the name.
                auto clip_bytes = [](std::string& s, std::size_t nb) {
                    if (s.size() <= nb) return false;
                    s.resize(nb);
                    while (!s.empty() &&
                           (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
                        s.pop_back();
                    return true;
                };
                std::string content;
                std::vector<StyledRun> runs;
                const std::size_t budget = static_cast<std::size_t>(name_w);

                // ══ THE FLOW TREE ═══════════════════════════════════════════
                // A radically different tree: instead of dead box-drawing
                // guides, the hierarchy's connective tissue CARRIES DATA.
                //  1. Weight gutter (1 cell): a block glyph sized by this
                //     node's CPU share among its siblings and colored on the
                //     load ramp — the busiest child at EVERY branch stands tall
                //     and bright, so you see which sibling dominates without
                //     reading a number or expanding anything.
                //  2. Heat-graded rails: the ├─ └─ │ guides are tinted by the
                //     subtree CPU flowing through that branch — the rail glows
                //     warm where load lives, so the eye follows the bright line
                //     straight down to the hog. The tree becomes a live heatmap.
                bool tree_row = view_.tree && idx >= 0 &&
                                idx < static_cast<int>(view_.tree_prefix.size());
                if (tree_row) {
                    const std::size_t I = static_cast<std::size_t>(idx);
                    std::string guide = view_.tree_prefix[I];
                    const bool kids = idx < static_cast<int>(view_.has_kids.size())
                                      && view_.has_kids[I];
                    const bool folded = idx < static_cast<int>(view_.collapsed_row.size())
                                        && view_.collapsed_row[I];
                    const double share = idx < static_cast<int>(view_.sib_share.size())
                                         ? view_.sib_share[I] : 0.0;
                    const double scpu = idx < static_cast<int>(view_.sub_cpu.size())
                                        ? view_.sub_cpu[I] : 0.0;

                    // Heat of this branch: subtree CPU on the load ramp, with a
                    // dim floor so idle branches recede to structure.
                    const bool warm = scpu >= 0.5;
                    Color heat = warm
                        ? lift(load_color(std::clamp(scpu / 100.0, 0.0, 1.0)))
                        : (selected ? mix(pal::label, pal::white, 0.2)
                                    : mix(pal::dim, pal::bg_panel, 0.25));

                    // (1) Weight gutter — always one cell, even at the root, so
                    // every row shares a left edge and the bars form a column.
                    static const char* kW[] = {"▁","▂","▃","▄","▅","▆","▇","█"};
                    int wl = std::clamp(static_cast<int>(share * 7.999), 0, 7);
                    std::string gut = kW[wl];
                    Color gut_c = warm ? heat
                                       : (selected ? pal::label
                                                   : mix(pal::dim, pal::bg_panel, 0.4));
                    if (content.size() + gut.size() <= budget) {
                        runs.push_back({content.size(), gut.size(),
                                        warm && scpu > 40 ? Style{}.with_fg(gut_c).with_bold()
                                                          : Style{}.with_fg(gut_c)});
                        content += gut;
                    }
                    if (content.size() < budget) content += " ";

                    // (2) Heat-graded rails: emit the accumulated guide, tinted
                    // by this branch's heat rather than a dead gray.
                    if (!guide.empty() && content.size() < budget) {
                        std::string g = guide;
                        clip_bytes(g, budget - content.size());
                        runs.push_back({content.size(), g.size(), Style{}.with_fg(heat)});
                        content += g;
                    }

                    // (3) Chevron for foldable nodes, in the branch heat; the
                    // collapsed marker doubles as "work is hiding here".
                    if (kids && content.size() < budget) {
                        std::string chev = folded ? "▸ " : "▾ ";
                        clip_bytes(chev, budget - content.size());
                        if (!chev.empty()) {
                            runs.push_back({content.size(), chev.size(),
                                            warm ? Style{}.with_fg(heat).with_bold()
                                                 : Style{}.with_fg(heat)});
                            content += chev;
                        }
                    }
                }

                std::string nm = std::string(fmt::clip(p.name, 32));
                const std::size_t off0 = content.size();
                if (off0 < budget) {
                    clip_bytes(nm, budget - off0);
                    runs.push_back({off0, nm.size(), name_st});
                    content += nm;
                }

                // Collapsed subtree count badge: " +N".
                if (tree_row && idx < static_cast<int>(view_.collapsed_row.size())
                    && view_.collapsed_row[static_cast<std::size_t>(idx)]
                    && idx < static_cast<int>(view_.hidden_count.size())) {
                    const int hc = view_.hidden_count[static_cast<std::size_t>(idx)];
                    std::string badge = "  +" + std::to_string(hc);
                    if (content.size() + badge.size() <= budget) {
                        std::size_t off = content.size();
                        content += badge;
                        runs.push_back({off, badge.size(),
                                        Style{}.with_fg(selected ? pal::label : pal::dim)});
                    }
                }

                if (!cmd_trail.empty() && content.size() + 4 < budget && !tree_row) {
                    std::size_t off = content.size();
                    std::string t = "  " + cmd_trail;
                    if (clip_bytes(t, budget - off - 3)) t += "…";
                    content += t;
                    runs.push_back({off, content.size() - off,
                                    Style{}.with_fg(selected ? pal::label
                                                             : mix(pal::dim, pal::bg_panel, 0.35))});
                }
                cols.push_back(Element{TextElement{.content = std::move(content), .style = {},
                                                   .wrap = TextWrap::NoWrap,
                                                   .runs = std::move(runs)}} | width(name_w));
            }
            if (show_port)
                cols.push_back((text(port_txt, sk == SortKey::Port ? cell_st(pal::sky).with_bold()
                                                                   : cell_st(pal::sky))
                                | nowrap | w_<9> | justify(Justify::End)).build());
            cols.push_back(Meter{disp_cpu_frac}.width(show_mem ? 14 : 8).groove(false).build_fixed());
            cols.push_back((text(cpu_txt, cpu_st) | nowrap | w_<6> | justify(Justify::End)).build());
            cols.push_back((text(humanize_bytes(static_cast<std::uint64_t>(disp_rss)),
                                 sk == SortKey::Mem ? cell_st(pal::white).with_bold()
                                                    : cell_st(pal::text))
                            | nowrap | w_<8> | justify(Justify::End)).build());
            if (show_memp)
                cols.push_back((text(memp_txt,
                                     cell_st(mem_frac > 0.1 ? pal::hot : memp_zero ? hushed : quiet))
                                | nowrap | w_<5> | justify(Justify::End)).build());
            if (show_io)
                cols.push_back((text(io_txt,
                                     sk == SortKey::Io && iorate > 512
                                         ? cell_st(pal::sky).with_bold()
                                         : cell_st(io_c))
                                | nowrap | w_<8> | justify(Justify::End)).build());
            cols.push_back((text(dot) | nowrap | fgc(dot_c) | w_<2> | justify(Justify::Center)).build());
            if (show_thr)
                cols.push_back((text(std::to_string(p.threads), cell_st(quiet))
                                | nowrap | w_<4> | justify(Justify::End)).build());
            return h(std::move(cols)) | gap(1);
        }();

        (void)alt;
        // Every row carries a hit id keyed by its filtered-view index, so a
        // click resolves to the exact row the renderer painted — no scroll_
        // start arithmetic in the mouse handler.
        // Selected row rides a full-width background strip (footer idiom:
        // bgc on the h-container paints the whole band; ambient-bg
        // inheritance carries it under every glyph). The strip is tinted a
        // whisper toward the proc accent so it reads as "selection", not
        // generic gray — together with the ▎ edge bar it's unmissable in
        // peripheral vision without any bold shouting.
        if (selected)
            return (std::move(row) | bgc(mix(pal::sel_bg, pal::proc_ac, 0.10))
                    | hit(hit_proc_row(idx))).build();
        return (std::move(row) | hit(hit_proc_row(idx))).build();
    }
};

}  // namespace rockbottom::ui
