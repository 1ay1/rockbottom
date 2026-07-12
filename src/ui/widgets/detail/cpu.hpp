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

    // In ultrawide mode the pane splits into two side-by-side columns so the
    // screen fills horizontally instead of one tall scrolling column. `L`
    // collects the left column (load graph + live stats + distribution), `R`
    // the right (per-core meters + top consumers + sensors). In normal mode
    // both point at the same vector and everything stacks as before.
    std::vector<Element> single;
    std::vector<Element> left, right;
    const bool split = cx.ultrawide;
    std::vector<Element>& L = split ? left : single;
    std::vector<Element>& R = split ? right : single;

    // ── hero: BIG number + graph ────────────────────────────────────────
    // Grafana stat-panel idiom: the headline figure in block digits with a
    // trend arrow, parked left of the load graph — readable across the room.
    L.push_back(section("LOAD OVER TIME", pal::cpu_ac));
    {
        const int gh = cx.graph_h;
        L.push_back((h(
            stat_card(c.total.v, load_color(c.total.v), "cpu load",
                      c.total_history.data(), c.total_hist_len, gh),
            y_axis(gh, 100.0, 3),
            Element{Graph{c.total_history.data(), c.total_hist_len}.fill().rows(gh)} | grow(1)
        ) | gap(1) | height(gh)).build());
    }
    L.push_back(gap_row());

    // ── right-now stat strip ─────────────────────────────────────────────────
    L.push_back(section("RIGHT NOW", pal::cpu_ac));
    L.push_back(bar("total", c.total.v, "busy across all cores", load_color(c.total.v), cx.wide ? 34 : 0));
    // User/system split — the first question about a busy CPU: is it MY code
    // or the kernel? Heavy system time usually means syscall/IO churn.
    if (c.user.v > 0 || c.system.v > 0) {
        L.push_back(bar("user", c.user.v, "running app code", pal::cpu_ac, cx.wide ? 34 : 0));
        L.push_back(bar("system", c.system.v, "in the kernel (syscalls)", pal::hot, cx.wide ? 34 : 0));
    }
    if (c.iowait.v > 0.005)
        L.push_back(bar("iowait", c.iowait.v, "stalled waiting on disk", pal::hot, cx.wide ? 34 : 0));

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
    L.push_back(kv3(
        "load 1m", fmt::fixed2(c.loadavg[0]), load_color(std::min(1.0, sat)),
        "5m", fmt::fixed2(c.loadavg[1]), pal::label,
        "15m", fmt::fixed2(c.loadavg[2]), pal::label));
    // Core topology reads "8 (4P + 4E)" on heterogeneous silicon.
    std::string topo = std::to_string(c.logical);
    if (c.perf_cores > 0 && c.eff_cores > 0)
        topo += " (" + std::to_string(c.perf_cores) + "P + " + std::to_string(c.eff_cores) + "E)";
    L.push_back(kv3(
        "logical cpus", topo, pal::text,
        "load / core", fmt::fixed2(sat), vc,
        c.temp_c > 1 ? "package" : "", c.temp_c > 1 ? std::to_string(static_cast<int>(c.temp_c)) + " °C" : "",
        load_color(std::clamp((c.temp_c - 40) / 50.0, 0.0, 1.0))));
    L.push_back(verdict(verdict_txt, vc));
    L.push_back(gap_row());

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
        L.push_back(section("DISTRIBUTION", pal::cpu_ac));
        L.push_back(kv3(
            "busiest core", fmt::pct(hi), load_color(hi),
            "quietest", fmt::pct(lo), load_color(lo),
            "median", fmt::pct(med), load_color(med)));
        L.push_back(kv3(
            "average", fmt::pct(avg), load_color(avg),
            "spread", fmt::pct(hi - lo), hi - lo > 0.5 ? pal::hot : pal::dim,
            "active cores", std::to_string(active) + "/" + std::to_string(n),
            active > n / 2 ? pal::hot : pal::good));
        if (hi - lo > 0.6 && hi > 0.8)
            L.push_back(verdict("▲ load is lopsided — one core is pinned while others idle "
                                "(a single-threaded hog?)", pal::hot));
        L.push_back(gap_row());
    }

    // ── per-core meters ──────────────────────────────────────────────────
    // On Apple Silicon macOS enumerates the efficiency cluster first (M1:
    // cpu0-3 = E, cpu4-7 = P). Tag each core with its cluster so "why is
    // core 6 pinned" answers itself; P-core ids get the brighter accent.
    R.push_back(section("PER-CORE", pal::cpu_ac,
                        c.perf_cores > 0 && c.eff_cores > 0
                            ? std::to_string(c.perf_cores) + "P + " + std::to_string(c.eff_cores) + "E"
                            : std::to_string(static_cast<int>(c.cores.size())) + " cores"));
    const int n = static_cast<int>(c.cores.size());
    const bool hetero = c.perf_cores > 0 && c.eff_cores > 0 &&
                        c.perf_cores + c.eff_cores == n;
    // Responsive column count. In split mode the per-core block owns only HALF
    // the pane width, so use fewer columns; single-column uses the full width.
    const int core_w = split ? cx.w / 2 : cx.w;
    int cols = core_w >= 140 ? 4 : core_w >= 104 ? 3 : core_w >= 68 ? 2 : 1;
    if (n <= 4) cols = 1;
    else if (n <= 8 && cols > 2) cols = 2;
    // A per-core row reads BEST when it can carry its own history sparkline
    // (id + meter + spark + % + freq — needs ~40 cells). Column count above
    // maximizes density, but in the ultrawide SPLIT layout the per-core block
    // owns only half the pane and the OTHER half runs out of content long
    // before this one does — leaving a tall band of dead space below the split.
    // So when we're split and a narrower grid would let every core show its
    // spark, step DOWN one column: fewer, richer rows that stack TALLER fill
    // that vertical room with real per-core trend history instead of blanks.
    if (split && cols > 1 && core_w / cols < 40 && core_w / (cols - 1) >= 40)
        --cols;
    const int per = (n + cols - 1) / cols;
    // Spark width scales with the room each column actually has — a wide split
    // column gets a longer history trace, a tight one the compact 12-cell run.
    const int spark_cells = core_w / std::max(1, cols) >= 56 ? 18 : 12;
    for (int r = 0; r < per; ++r) {
        std::vector<Element> line;
        for (int col = 0; col < cols; ++col) {
            int i = col * per + r;
            if (i >= n) { line.push_back(Element{blank()} | grow(1)); continue; }
            const CpuCore& core = c.cores[static_cast<std::size_t>(i)];
            const double f = core.usage.v;
            char id[10];
            if (hetero)
                std::snprintf(id, sizeof id, "%2d·%c", i, i < c.eff_cores ? 'E' : 'P');
            else
                std::snprintf(id, sizeof id, "%2d", i);
            std::string fq = core.freq.value > 0
                ? fmt::fixed2(static_cast<double>(core.freq.value) / 1e9) + "G" : "";
            // Per-core load sparkline: the core's own recent history, so you
            // see WHICH cores have been busy over time, not just this instant.
            // Shown when the column is wide enough to carry a meter + a spark.
            const bool room = core_w / cols >= 40;
            line.push_back(Element{(h(
                text(id) | nowrap | fgc(hetero && i >= c.eff_cores
                                            ? pal::cpu_ac : mix(pal::cpu_ac, pal::dim, 0.5))
                    | width(hetero ? 5 : 3),
                Element{Meter{f}.fill().groove(false)} | grow(1),
                room ? Spark{core.history.data(), core.hist_len}.cells(spark_cells).build_fixed()
                     : Element{blank()} | width(0),
                text(fmt::pct_pad(f)) | nowrap | fgc(load_color(f)) | width(5) | justify(Justify::End),
                text(fq) | nowrap | fgc(pal::faint) | width(6) | justify(Justify::End)
            ) | gap(1)).build()} | grow(1));
        }
        R.push_back((h(line) | gap(3)).build());
    }
    R.push_back(gap_row());

    // ── top CPU consumers ──────────────────────────────────────────────────
    // The question a hot CPU pane exists to answer: WHO. Same ranked-list
    // grid as the memory / disk panes.
    {
        std::vector<const ProcInfo*> top;
        for (const auto& p : s.procs) top.push_back(&p);
        std::sort(top.begin(), top.end(),
                  [](const ProcInfo* a, const ProcInfo* b2) { return a->cpu > b2->cpu; });
        // On a tall ultrawide the right column has room to spare below the
        // per-core grid — show a deeper top-N there instead of blank rows.
        const int cap = split && cx.tall ? 12 : cx.tall ? 8 : 4;
        const int show = std::min<int>(cap, static_cast<int>(top.size()));
        R.push_back(section("TOP CPU CONSUMERS", pal::cpu_ac, "top " + std::to_string(show)));
        for (int i = 0; i < show; ++i) {
            const ProcInfo& p = *top[static_cast<std::size_t>(i)];
            const double f = std::clamp(p.cpu / 100.0, 0.0, 1.0);
            char pct[16]; std::snprintf(pct, sizeof pct, "%5.1f%%", p.cpu);
            R.push_back(rank_row(i + 1, std::to_string(p.pid), std::string(fmt::clip(p.name, 22)),
                                 f, pal::cpu_ac,
                                 pct, load_color(f), 7));
        }
    }

    // ── sensors ───────────────────────────────────────────────────
    // Hardware temperatures from hwmon (Linux): CPU package/cores, NVMe drives,
    // chipset, battery, wifi — the readings you'd otherwise shell out to
    // `sensors` for. Grouped by zone, each with a small heat bar. Empty on
    // macOS (no public temperature API), so the section just doesn't appear.
    if (!s.sensors.empty()) {
        R.push_back(gap_row());
        R.push_back(section("SENSORS", pal::cpu_ac,
                            std::to_string(s.sensors.size()) + " probes"));
        std::string cur_zone = "\x01";   // sentinel so the first row prints its zone
        for (const Sensor& sn : s.sensors) {
            if (sn.zone != cur_zone) {
                cur_zone = sn.zone;
                R.push_back((text("  " + cur_zone) | nowrap | fgc(pal::faint)).build());
            }
            // Heat fraction: 30°C floor → crit (or 95°C) ceiling on the load ramp.
            // Some sensors report garbage thresholds (e.g. 65261°C on nvme
            // "Sensor 1") — only trust values in a plausible 40..150°C band.
            auto sane = [](float v) { return v > 40 && v < 150; };
            const float ceil = sane(sn.crit_c) ? sn.crit_c : 95.0f;
            const double frac = std::clamp((sn.temp_c - 30.0) / (ceil - 30.0), 0.0, 1.0);
            char t[16]; std::snprintf(t, sizeof t, "%.0f°C", sn.temp_c);
            std::string tail = sane(sn.high_c)
                ? ("high " + std::to_string(static_cast<int>(sn.high_c)) + "°")
                : (sane(sn.crit_c) ? "crit " + std::to_string(static_cast<int>(sn.crit_c)) + "°" : "");
            R.push_back((h(
                text("    " + std::string(fmt::clip(sn.label, 18))) | nowrap | fgc(pal::label) | width(20),
                Element{Meter{frac}.fill().groove(false).color(load_color(frac))} | grow(1),
                text(t) | nowrap | Bold | fgc(load_color(frac)) | width(7) | justify(Justify::End),
                text(tail) | nowrap | fgc(pal::faint) | width(11) | justify(Justify::End)
            ) | gap(1)).build());
        }
    }

    if (split) return two_col(std::move(left), std::move(right));
    return single;
}

}  // namespace rockbottom::ui::detail
