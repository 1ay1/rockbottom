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

namespace bottom::ui {

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

        // Kill confirmation strip replaces the header while pending.
        if (view_.pending) rows.push_back(confirm_strip());
        else               rows.push_back(header_row());

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

        // Scroll window that keeps the selection visible.
        const int body_rows = std::max(3, view_.max_rows - 1);
        int start = 0;
        if (view_.selected >= body_rows) start = view_.selected - body_rows + 1;
        start = std::clamp(start, 0, std::max(0, n - body_rows));

        std::vector<Element> body;
        for (int i = start; i < n && i < start + body_rows; ++i)
            body.push_back(proc_row(*procs[static_cast<std::size_t>(i)],
                                    i == view_.selected,
                                    loud && i == hi));

        if (n == 0)
            body.push_back((text(view_.filter.empty()
                                     ? "no processes"
                                     : "nothing matches \"" + view_.filter + "\"")
                            | fgc(pal::dim)).build());

        // Scrollbar: maya's scrollbar_y renders from plain ScrollState fields,
        // so we drive it straight from our selection window — no scroll
        // plumbing, just where-am-I feedback for 380-row lists.
        if (n > body_rows) {
            ScrollState sb;
            sb.y = start;
            sb.max_y = n - body_rows;
            ScrollbarStyle st;
            st.track_color = pal::faint;
            st.thumb_color = pal::proc_ac;
            rows.push_back((h(
                (v(body) | grow(1)),
                scrollbar_y(sb, static_cast<int>(body.size()), st)
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

    [[nodiscard]] maya::Element header_row() const {
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
            return text(s) | nowrap | Bold | fgc(c);
        };
        std::vector<Element> cols;
        cols.push_back(((text("  PID") | nowrap | Bold | fgc(pal::dim)) | w_<8>).build());
        cols.push_back(((text("USER") | nowrap | Bold | fgc(pal::dim)) | w_<8>).build());
        cols.push_back((hdr("NAME", SortKey::Name) | grow(1)).build());
        if (show_port) cols.push_back((hdr("PORT", SortKey::Port) | w_<11>).build());
        cols.push_back((hdr("CPU", SortKey::Cpu) | width(show_mem ? 21 : 14)).build());
        cols.push_back((hdr("MEM", SortKey::Mem) | width(show_mem ? 20 : 8)).build());
        if (show_memp) cols.push_back(((text("MEM%") | nowrap | Bold | fgc(pal::dim)) | w_<5>).build());
        if (show_io) cols.push_back((hdr("DISK", SortKey::Io) | w_<10>).build());
        cols.push_back(((text("S") | nowrap | Bold | fgc(pal::dim)) | w_<2>).build());
        if (show_thr) cols.push_back((text("THR") | nowrap | Bold | fgc(pal::dim)).build());
        return (h(std::move(cols)) | gap(1)).build();
    }

    [[nodiscard]] maya::Element proc_row(const ProcInfo& p, bool selected, bool culprit) const {
        using namespace maya;
        using namespace maya::dsl;

        const double cpu_frac = std::clamp(p.cpu / 100.0, 0.0, 1.0);
        const double mem_frac = p.mem_share.v;

        Style name_st = Style{}.with_fg(culprit ? pal::crit : selected ? pal::white : pal::text);
        if (culprit || selected) name_st = name_st.with_bold();
        Style cpu_st = Style{}.with_fg(load_color(cpu_frac));
        if (p.cpu > 50) cpu_st = cpu_st.with_bold();

        const char* dot = p.state == 'R' ? "●" : p.state == 'D' ? "◆" : "·";
        Color dot_c = p.state == 'R' ? pal::good
                    : p.state == 'D' ? pal::crit : pal::faint;

        // Selection cursor beats culprit marker in the gutter; culprit keeps
        // its red name so it stays identifiable while selected.
        std::string gutter = selected ? "▶" : culprit ? "»" : " ";
        Color gutter_c = selected ? pal::proc_ac : culprit ? pal::crit : pal::dim;

        char cpu_txt[16];
        std::snprintf(cpu_txt, sizeof cpu_txt, "%5.1f", p.cpu);
        char memp_txt[16];
        std::snprintf(memp_txt, sizeof memp_txt, "%4.1f", p.mem_share.percent());

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
            Color io_c = iorate > 512 ? pal::sky : pal::faint;
            std::vector<Element> cols;
            cols.push_back((text(gutter + std::to_string(p.pid)) | nowrap | fgc(gutter_c) | w_<8>).build());
            cols.push_back((text(fmt::clip(p.user, 7)) | nowrap | fgc(pal::label) | w_<8>).build());
            cols.push_back((text(fmt::clip(p.name, 32), name_st) | nowrap | grow(1)).build());
            if (show_port)
                cols.push_back((text(port_txt) | nowrap | fgc(pal::sky) | w_<11>).build());
            cols.push_back(Meter{cpu_frac}.width(show_mem ? 14 : 8).build_fixed());
            cols.push_back((text(cpu_txt, cpu_st) | nowrap | w_<6>).build());
            if (show_mem)
                cols.push_back(Meter{mem_frac}.width(12).color(pal::mem_ac).build_fixed());
            cols.push_back((text(humanize_bytes(p.rss)) | nowrap | fgc(pal::text) | w_<7>).build());
            if (show_memp)
                cols.push_back((text(memp_txt) | nowrap | fgc(mem_frac > 0.1 ? pal::hot : pal::dim) | w_<5>).build());
            if (show_io)
                cols.push_back((text(io_txt) | nowrap | fgc(io_c) | w_<10>).build());
            cols.push_back((text(dot) | nowrap | fgc(dot_c) | w_<2>).build());
            if (show_thr)
                cols.push_back((text(std::to_string(p.threads)) | nowrap | fgc(pal::dim)).build());
            return h(std::move(cols)) | gap(1);
        }();

        if (selected) return (std::move(row) | bgc(pal::bg_panel)).build();
        return row.build();
    }
};

}  // namespace bottom::ui
