// widgets/proc_panel.hpp — interactive process table.
//
// Renders the filtered/sorted process list with:
//   • a selection bar (▶ + tinted background) driven by ↑/↓ j/k
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
                                    name_w));

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

        // Chip: filter beats sort in relevance when active.
        std::string chip = view_.filtering ? "/" + view_.filter + "▌"
                         : !view_.filter.empty() ? "/" + view_.filter + " · " + sort_label()
                         : "sort " + sort_label() + " · " + std::to_string(n);

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
        Color c = hard ? pal::crit : pal::warn;
        std::string q = std::string(hard ? "force-kill " : "end ") +
                        p.name + " (" + std::to_string(p.pid) + ")?";
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
        auto hdr = [&](const char* name, SortKey self) {
            std::string s = view_.sort == self ? std::string(name) + "▾" : std::string(name);
            Color c = view_.sort == self ? pal::proc_ac : pal::dim;
            return text(s) | nowrap | Bold | Underline | fgc(c);
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
        auto plain_num = [&](const char* name, int num_w) {
            return ((text(name) | nowrap | Bold | Underline | fgc(pal::dim)) | width(num_w)
                    | justify(Justify::End)).build();
        };
        std::vector<Element> cols;
        cols.push_back(((text("  PID") | nowrap | Bold | Underline | fgc(pal::dim)) | w_<8>).build());
        cols.push_back(((text("USER") | nowrap | Bold | Underline | fgc(pal::dim)) | w_<8>).build());
        cols.push_back((hdr("NAME", SortKey::Name) | grow(1)).build());
        if (show_port) cols.push_back((hdr("PORT", SortKey::Port) | w_<9> | justify(Justify::End)).build());
        cols.push_back(num_hdr("CPU", SortKey::Cpu, show_mem ? 14 : 8, 6));
        cols.push_back((hdr("MEM", SortKey::Mem) | w_<8> | justify(Justify::End)).build());
        if (show_memp) cols.push_back(plain_num("MEM%", 5));
        if (show_io) cols.push_back((hdr("DISK", SortKey::Io) | w_<8> | justify(Justify::End)).build());
        cols.push_back(((text("S") | nowrap | Bold | Underline | fgc(pal::dim)) | w_<2> | justify(Justify::Center)).build());
        if (show_thr) cols.push_back(((text("THR") | nowrap | Bold | Underline | fgc(pal::dim)) | w_<4> | justify(Justify::End)).build());
        if (rgutter > 0) cols.push_back((Element{blank()} | width(rgutter)).build());
        return (h(std::move(cols)) | gap(1)).build();
    }

    [[nodiscard]] maya::Element proc_row(const ProcInfo& p, bool selected, bool culprit,
                                         bool alt, int name_w) const {
        using namespace maya;
        using namespace maya::dsl;

        const double cpu_frac = std::clamp(p.cpu / 100.0, 0.0, 1.0);
        const double mem_frac = p.mem_share.v;

        // The cursor row is "lit" purely with ink — NO background paint —
        // every cell's hue lifts toward white and goes bold, so the whole
        // line visibly glows against its dim peers while colors keep their
        // meaning (a hot cpu figure stays warm, just brighter).
        auto lift = [&](Color c) { return selected ? mix(c, pal::white, 0.4) : c; };
        auto cell_st = [&](Color c) {
            Style st = Style{}.with_fg(lift(c));
            if (selected) st = st.with_bold();
            return st;
        };
        const Color quiet  = selected ? pal::text  : pal::dim;    // dim ink, lifted
        const Color hushed = selected ? pal::label : pal::faint;  // faint ink, lifted

        Style name_st = Style{}.with_fg(culprit ? pal::crit : selected ? pal::white : pal::text);
        if (culprit || selected) name_st = name_st.with_bold();
        Style cpu_st = cell_st(load_color(cpu_frac));
        if (p.cpu > 50) cpu_st = cpu_st.with_bold();

        const char* dot = p.state == 'R' ? "●" : p.state == 'D' ? "◆" : "·";
        Color dot_c = lift(p.state == 'R' ? pal::good
                     : p.state == 'D' ? pal::crit : pal::faint);

        // The active sort column's values get brighter ink so the column the
        // table is ranked by reads as the "spine" of the list.
        const SortKey sk = view_.sort;

        // root-owned rows get a quiet warm tint on the user column — enough
        // to pick out privileged processes without shouting.
        Color user_c = p.user == "root" ? mix(pal::hot, pal::label, 0.55) : pal::label;

        // Selection cursor beats culprit marker in the gutter; culprit keeps
        // its red name so it stays identifiable while selected.
        std::string gutter = selected ? "▶" : culprit ? "»" : " ";
        Color gutter_c = selected ? pal::proc_ac : culprit ? pal::crit : pal::dim;

        char cpu_txt[16];
        std::snprintf(cpu_txt, sizeof cpu_txt, "%5.1f", p.cpu);
        char memp_txt[16];
        std::snprintf(memp_txt, sizeof memp_txt, "%4.1f", p.mem_share.percent());
        // Zero reads as silence: idle figures drop to faint ink so the
        // columns aren't a wall of identical 0.0s and live rows pop.
        const bool cpu_zero  = p.cpu < 0.05;
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
            Style pid_st = Style{}.with_fg(selected ? pal::proc_ac
                                          : sk == SortKey::Pid ? pal::text : gutter_c);
            if (selected) pid_st = pid_st.with_bold();
            cols.push_back((text(gutter + std::to_string(p.pid), pid_st) | nowrap | w_<8>).build());
            cols.push_back((text(fmt::clip(p.user, 7), cell_st(user_c)) | nowrap | w_<8>).build());
            {
                // name (styled) + dim command trail, hard-clipped to the
                // analytically computed cell width so fixed columns to the
                // right never shift.
                auto clip_bytes = [](std::string& s, std::size_t nb) {
                    if (s.size() <= nb) return false;
                    s.resize(nb);
                    while (!s.empty() &&
                           (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
                        s.pop_back();
                    return true;
                };
                std::string content = std::string(fmt::clip(p.name, 32));
                std::vector<StyledRun> runs;
                const std::size_t budget = static_cast<std::size_t>(name_w);
                clip_bytes(content, budget);
                runs.push_back({0, content.size(), name_st});
                if (!cmd_trail.empty() && content.size() + 4 < budget) {
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
            cols.push_back(Meter{cpu_frac}.width(show_mem ? 14 : 8).groove(false).build_fixed());
            cols.push_back((text(cpu_txt, cpu_st) | nowrap | w_<6> | justify(Justify::End)).build());
            cols.push_back((text(humanize_bytes(p.rss),
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
        return row.build();
    }
};

}  // namespace rockbottom::ui
