// widgets/proc_panel.hpp — process table: top consumers by the active sort
// key, with an inline mini-meter per row (instantly scannable), state dots,
// and the culprit row flagged with » in the health color.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../../core/sampler.hpp"   // SortKey
#include "../theme.hpp"
#include "../fmt.hpp"
#include "meter.hpp"
#include "panel.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace bottom::ui {

class ProcPanel {
    const Snapshot& snap_;
    SortKey sort_;
    int max_rows_;

public:
    ProcPanel(const Snapshot& s, SortKey sort, int max_rows)
        : snap_(s), sort_(sort), max_rows_(std::max(4, max_rows)) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;
        rows.push_back(header_row());

        // Culprit = loudest row by the active key, if it's actually loud.
        int hi = -1;
        double best = -1;
        for (std::size_t i = 0; i < snap_.procs.size(); ++i) {
            double v = sort_ == SortKey::Mem
                           ? static_cast<double>(snap_.procs[i].rss.value)
                           : snap_.procs[i].cpu;
            if (v > best) { best = v; hi = static_cast<int>(i); }
        }
        const bool loud = (sort_ == SortKey::Mem)
            ? (hi >= 0 && snap_.procs[static_cast<std::size_t>(hi)].mem_share.percent() > 8)
            : (hi >= 0 && best > 25);

        const int limit = std::min<int>(max_rows_, static_cast<int>(snap_.procs.size()));
        for (int i = 0; i < limit; ++i)
            rows.push_back(proc_row(snap_.procs[static_cast<std::size_t>(i)],
                                    loud && i == hi));

        return Panel("≡", "PROCESSES", pal::proc_ac)
            .chip("sort " + sort_label())
            .grow(1)(std::move(rows));
    }

private:
    [[nodiscard]] std::string sort_label() const {
        switch (sort_) {
            case SortKey::Cpu:  return "cpu";
            case SortKey::Mem:  return "mem";
            case SortKey::Pid:  return "pid";
            case SortKey::Name: return "name";
        }
        return "cpu";
    }

    [[nodiscard]] maya::Element header_row() const {
        using namespace maya;
        using namespace maya::dsl;
        auto hdr = [&](const char* name, SortKey self, int) {
            std::string s = sort_ == self ? std::string(name) + "▾" : std::string(name);
            Color c = sort_ == self ? pal::proc_ac : pal::dim;
            return text(s) | nowrap | Bold | fgc(c);
        };
        return (h(
            (text("PID") | nowrap | Bold | fgc(pal::dim)) | w_<7>,
            (text("USER") | nowrap | Bold | fgc(pal::dim)) | w_<7>,
            hdr("NAME", SortKey::Name, 0) | w_<15>,
            hdr("CPU", SortKey::Cpu, 0) | w_<12>,
            hdr("MEM", SortKey::Mem, 0) | w_<6>,
            (text("S") | nowrap | Bold | fgc(pal::dim)) | w_<2>
        ) | gap(1)).build();
    }

    [[nodiscard]] maya::Element proc_row(const ProcInfo& p, bool culprit) const {
        using namespace maya;
        using namespace maya::dsl;

        const double cpu_frac = std::clamp(p.cpu / 100.0, 0.0, 1.0);

        Style name_st = Style{}.with_fg(culprit ? pal::crit : pal::text);
        if (culprit) name_st = name_st.with_bold();
        Style cpu_st = Style{}.with_fg(load_color(cpu_frac));
        if (p.cpu > 50) cpu_st = cpu_st.with_bold();

        // State dot: running green, disk-sleep red, else dim.
        const char* dot = p.state == 'R' ? "●" : p.state == 'D' ? "◆" : "·";
        Color dot_c = p.state == 'R' ? pal::good
                    : p.state == 'D' ? pal::crit : pal::faint;

        std::string mark = culprit ? "»" : " ";
        char cpu_txt[16];
        std::snprintf(cpu_txt, sizeof cpu_txt, "%5.1f", p.cpu);

        return (h(
            text(mark + std::to_string(p.pid))
                | nowrap | fgc(culprit ? pal::crit : pal::dim) | w_<7>,
            text(fmt::clip(p.user, 6)) | nowrap | fgc(pal::label) | w_<7>,
            text(fmt::clip(p.name, 14), name_st) | nowrap | w_<15>,
            Meter{cpu_frac}.width(5),
            text(cpu_txt, cpu_st) | nowrap | w_<6>,
            text(humanize_bytes(p.rss)) | nowrap | fgc(pal::text) | w_<6>,
            text(dot) | nowrap | fgc(dot_c) | w_<2>
        ) | gap(1)).build();
    }
};

}  // namespace bottom::ui
