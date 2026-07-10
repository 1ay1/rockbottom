// widgets/detail/cpu.hpp — the CPU drill-down body.
//
// More than btop's per-core strip: a hero load graph with a real y-axis, a
// distribution strip (min / median / max / spread across cores), a saturation
// verdict that compares load-average to core count, package temperature, and
// every logical core as its own labelled meter with live frequency.

#pragma once

#include "common.hpp"

namespace rockbottom::ui::detail {

// Returns the body rows for the CPU pane (unframed, unscrolled).
inline std::vector<Element> cpu_body(const Snapshot& s, const Ctx& cx) {
    using namespace maya; using namespace maya::dsl;
    const CpuInfo& c = s.cpu;
    std::vector<Element> b;

    // ── hero graph ─────────────────────────────────────────────────────────────────
    // Gauge idiom: the live total rides the section rule as a big bold
    // number with its own load hue — the one figure the pane is about.
    {
        std::vector<Element> hdr;
        hdr.push_back(Element{section("LOAD OVER TIME", pal::cpu_ac)} | grow(1));
        hdr.push_back((text("── cpu ") | nowrap | Bold | fgc(load_color(c.total.v))).build());
        hdr.push_back((text(" " + fmt::pct(c.total.v) + " ") | nowrap | Bold
                       | fgc(pal::bg) | bgc(load_color(c.total.v))).build());
        b.push_back((h(std::move(hdr)) | gap(1)).build());
    }
    {
        const int gh = cx.graph_h;
        std::vector<Element> axis;
        for (int r = 0; r < gh; ++r) {
            std::string lbl = "   ";
            if (r == 0) lbl = "100";
            else if (r == gh - 1) lbl = "  0";
            else if (r == gh / 2) lbl = " 50";
            axis.push_back((text(lbl) | nowrap | fgc(pal::faint)).build());
        }
        b.push_back((h(
            v(std::move(axis)) | width(3),
            Element{Graph{c.total_history.data(), c.total_hist_len}.fill().rows(gh)} | grow(1)
        ) | gap(1) | height(gh)).build());
    }
    b.push_back(gap_row());

    // ── right-now stat strip ─────────────────────────────────────────────────
    b.push_back(section("RIGHT NOW", pal::cpu_ac));
    b.push_back(bar("total", c.total.v, "busy across all cores", load_color(c.total.v), cx.wide ? 34 : 0));
    b.push_back(bar("iowait", c.iowait.v, "cores stalled waiting on disk", pal::hot, cx.wide ? 34 : 0));

    // Load average, interpreted against the core count — the number htop shows
    // but never explains. >1.0 per core = the run queue is backing up.
    const int lc = std::max(1, c.logical);
    const double sat = c.loadavg[0] / lc;
    const char* verdict_txt =
        sat < 0.7 ? "● plenty of headroom — nothing is queuing for a core"
      : sat < 1.0 ? "● comfortably busy — cores keeping up with demand"
      : sat < 2.0 ? "▲ oversubscribed — tasks are waiting for a free core"
      :             "▲ heavily saturated — the run queue is deep, things will feel slow";
    const maya::Color vc = sat < 0.7 ? pal::good : sat < 1.0 ? pal::teal
                         : sat < 2.0 ? pal::hot : pal::crit;
    b.push_back(kv3(
        "load 1m", fmt::fixed2(c.loadavg[0]), load_color(std::min(1.0, sat)),
        "5m", fmt::fixed2(c.loadavg[1]), pal::label,
        "15m", fmt::fixed2(c.loadavg[2]), pal::label));
    b.push_back(kv3(
        "logical cpus", std::to_string(c.logical), pal::text,
        "load / core", fmt::fixed2(sat), vc,
        c.temp_c > 1 ? "package" : "", c.temp_c > 1 ? std::to_string(static_cast<int>(c.temp_c)) + " °C" : "",
        load_color(std::clamp((c.temp_c - 40) / 50.0, 0.0, 1.0))));
    b.push_back(verdict(verdict_txt, vc));
    b.push_back(gap_row());

    // ── distribution across cores ────────────────────────────────────────────
    if (!c.cores.empty()) {
        std::vector<double> us;
        us.reserve(c.cores.size());
        for (const auto& core : c.cores) us.push_back(core.usage.v);
        std::sort(us.begin(), us.end());
        const int n = static_cast<int>(us.size());
        const double lo = us.front(), hi = us.back();
        const double med = us[n / 2];
        double sum = 0; int active = 0;
        for (double u : us) { sum += u; if (u > 0.05) ++active; }
        const double avg = sum / n;
        b.push_back(section("DISTRIBUTION", pal::cpu_ac));
        b.push_back(kv3(
            "busiest core", fmt::pct(hi), load_color(hi),
            "quietest", fmt::pct(lo), load_color(lo),
            "median", fmt::pct(med), load_color(med)));
        b.push_back(kv3(
            "average", fmt::pct(avg), load_color(avg),
            "spread", fmt::pct(hi - lo), hi - lo > 0.5 ? pal::hot : pal::dim,
            "active cores", std::to_string(active) + "/" + std::to_string(n),
            active > n / 2 ? pal::hot : pal::good));
        if (hi - lo > 0.6 && hi > 0.8)
            b.push_back(verdict("▲ load is lopsided — one core is pinned while others idle "
                                "(a single-threaded hog?)", pal::hot));
        b.push_back(gap_row());
    }

    // ── per-core meters ──────────────────────────────────────────────────────
    b.push_back(section("PER-CORE", pal::cpu_ac));
    const int n = static_cast<int>(c.cores.size());
    // Responsive column count: wider terminals fit more core columns.
    int cols = cx.w >= 140 ? 4 : cx.w >= 104 ? 3 : cx.w >= 68 ? 2 : 1;
    if (n <= 4) cols = 1;
    else if (n <= 8 && cols > 2) cols = 2;
    const int per = (n + cols - 1) / cols;
    for (int r = 0; r < per; ++r) {
        std::vector<Element> line;
        for (int col = 0; col < cols; ++col) {
            int i = col * per + r;
            if (i >= n) { line.push_back(Element{blank()} | grow(1)); continue; }
            const CpuCore& core = c.cores[static_cast<std::size_t>(i)];
            const double f = core.usage.v;
            char id[8]; std::snprintf(id, sizeof id, "%2d", i);
            std::string fq = core.freq.value > 0
                ? fmt::fixed2(static_cast<double>(core.freq.value) / 1e9) + "G" : "";
            line.push_back(Element{(h(
                text(id) | nowrap | fgc(pal::cpu_ac) | width(3),
                Element{Meter{f}.fill().groove(false)} | grow(1),
                text(fmt::pct_pad(f)) | nowrap | fgc(load_color(f)) | width(5) | justify(Justify::End),
                text(fq) | nowrap | fgc(pal::faint) | width(6) | justify(Justify::End)
            ) | gap(1)).build()} | grow(1));
        }
        b.push_back((h(line) | gap(3)).build());
    }

    return b;
}

}  // namespace rockbottom::ui::detail
