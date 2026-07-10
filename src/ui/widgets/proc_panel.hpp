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

        for (int i = start; i < n && i < start + body_rows; ++i)
            rows.push_back(proc_row(*procs[static_cast<std::size_t>(i)],
                                    i == view_.selected,
                                    loud && i == hi));

        if (n == 0)
            rows.push_back((text(view_.filter.empty()
                                     ? "no processes"
                                     : "nothing matches \"" + view_.filter + "\"")
                            | fgc(pal::dim)).build());

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
            case SortKey::Pid:  return "pid";
            case SortKey::Name: return "name";
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
        auto hdr = [&](const char* name, SortKey self) {
            std::string s = view_.sort == self ? std::string(name) + "▾" : std::string(name);
            Color c = view_.sort == self ? pal::proc_ac : pal::dim;
            return text(s) | nowrap | Bold | fgc(c);
        };
        return (h(
            (text("  PID") | nowrap | Bold | fgc(pal::dim)) | w_<8>,
            (text("USER") | nowrap | Bold | fgc(pal::dim)) | w_<7>,
            hdr("NAME", SortKey::Name) | w_<15>,
            hdr("CPU", SortKey::Cpu) | w_<12>,
            hdr("MEM", SortKey::Mem) | w_<6>,
            (text("S") | nowrap | Bold | fgc(pal::dim)) | w_<2>
        ) | gap(1)).build();
    }

    [[nodiscard]] maya::Element proc_row(const ProcInfo& p, bool selected, bool culprit) const {
        using namespace maya;
        using namespace maya::dsl;

        const double cpu_frac = std::clamp(p.cpu / 100.0, 0.0, 1.0);

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

        auto row = h(
            text(gutter + std::to_string(p.pid)) | nowrap | fgc(gutter_c) | w_<8>,
            text(fmt::clip(p.user, 6)) | nowrap | fgc(pal::label) | w_<7>,
            text(fmt::clip(p.name, 14), name_st) | nowrap | w_<15>,
            Meter{cpu_frac}.width(5),
            text(cpu_txt, cpu_st) | nowrap | w_<6>,
            text(humanize_bytes(p.rss)) | nowrap | fgc(pal::text) | w_<6>,
            text(dot) | nowrap | fgc(dot_c) | w_<2>
        ) | gap(1);

        if (selected) return (std::move(row) | bgc(pal::bg_panel)).build();
        return row.build();
    }
};

}  // namespace bottom::ui
