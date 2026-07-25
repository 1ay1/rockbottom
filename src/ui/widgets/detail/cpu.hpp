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
    std::vector<Element> hero, left, right;
    const bool split = cx.ultrawide;
    std::vector<Element>& L = split ? left : single;
    std::vector<Element>& R = split ? right : single;

    // ── hero: BIG number + graph ───────────────────────────────
    // Grafana stat-panel idiom: the headline figure in block digits with a
    // trend arrow, parked left of the load graph — readable across the room.
    // In split mode the hero rides the FULL pane width (hero_split) so the
    // trace spans the whole first band instead of a half-width sliver.
    std::vector<Element>& H = split ? hero : single;
    H.push_back(section("LOAD OVER TIME", pal::cpu_ac));
    {
        const int gh = cx.graph_h;
        H.push_back(hero_graph(c.total.v, load_color(c.total.v), "cpu load",
                               c.total_history.data(), c.total_hist_len, gh));
    }
    if (!split) L.push_back(gap_row());

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
    if (c.hetero())
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
        // On heterogeneous silicon the SINGLE most actionable number is where
        // the work landed: heavy load on the efficiency cluster while the
        // performance cluster idles means your job is running slow for no
        // reason. Show both cluster averages side by side, and say so plainly.
        if (c.hetero()) {
            double ps = 0, es = 0;
            int pn = 0, en = 0;
            for (const auto& core : c.cores) {
                if (core.kind == CoreKind::Eff)       { es += core.usage.v; ++en; }
                else if (core.kind == CoreKind::Perf) { ps += core.usage.v; ++pn; }
            }
            const double pavg = pn ? ps / pn : 0, eavg = en ? es / en : 0;
            L.push_back(kv3(
                "P cores", fmt::pct(pavg) + " avg", load_color(pavg),
                "E cores", fmt::pct(eavg) + " avg", load_color(eavg),
                "headroom", pn ? fmt::pct(1.0 - pavg) + " on P" : "",
                1.0 - pavg > 0.5 ? pal::good : pal::hot));
            if (eavg > 0.7 && pavg < 0.3)
                L.push_back(verdict("▲ the efficiency cores are doing the work while the "
                                    "performance cores idle — pin the hot process to a P core",
                                    pal::hot));
        }
        L.push_back(gap_row());
    }

    // ── per-core meters ────────────────────────────────────────
    // Every logical core: a load meter, its own history spark, load %, clock,
    // die temperature, and — on heterogeneous silicon — which cluster it
    // belongs to. The class and the temperature both arrive RESOLVED from the
    // sampler (CpuCore::kind / ::temp_c); this pane never parses a sensor
    // label or guesses a cluster layout from core counts.
    const int n = static_cast<int>(c.cores.size());
    const bool hetero = c.hetero();

    bool have_core_temp = false;
    for (const CpuCore& core : c.cores) if (core.temp_c > 0) { have_core_temp = true; break; }

    // Column legend in the section chip so the trailing bare figures read
    // unambiguously — a per-core row is "id  meter  spark  <load>%  <clock>GHz
    // [<temp>°C]", and without this the % / G / ° columns are unlabelled.
    // Cluster split (nP+mE) still leads when the chip has room.
    {
        std::string chip = hetero
            ? std::to_string(c.perf_cores) + "P + " + std::to_string(c.eff_cores) + "E · "
            : std::to_string(n) + " cores · ";
        chip += have_core_temp ? "load · GHz · °C" : "load · GHz";
        R.push_back(section("PER-CORE", pal::cpu_ac, chip));
    }
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
    const int col_w = core_w / std::max(1, cols);
    // Spark width scales with the room each column actually has — a wide split
    // column gets a longer history trace, a tight one the compact 12-cell run.
    const int spark_cells = col_w >= 56 ? 18 : 12;
    // ALIGNMENT CONTRACT for a core row: the meter is the ONLY elastic cell;
    // everything to its right is a fixed reserved column that is present on
    // EVERY row whether or not this particular core has a figure for it. The
    // original code dropped the temp cell to width(0) on cores with no probe,
    // which let the meter eat those cells and shifted that row's %/GHz left of
    // its neighbours' — the ragged grid in issue #2. Reserve-and-blank, never
    // drop, is what makes the columns line up.
    const bool show_spark = col_w >= 40;
    const bool show_temp_col = have_core_temp && col_w >= 44;
    const bool show_freq_col = [&] {
        for (const CpuCore& core : c.cores) if (core.freq.value > 0) return true;
        return false;
    }();
    const int id_w = hetero ? 5 : 3;

    // One core's row. Rendered identically for every core so the fixed columns
    // land at the same offset down the whole grid.
    auto core_row = [&](int i) -> Element {
        const CpuCore& core = c.cores[static_cast<std::size_t>(i)];
        const double f = core.usage.v;
        char id[12];
        if (hetero)
            std::snprintf(id, sizeof id, "%2d·%c", i, core.kind == CoreKind::Eff ? 'E' : 'P');
        else
            std::snprintf(id, sizeof id, "%2d", i);
        const std::string fq = core.freq.value > 0
            ? fmt::fixed2(static_cast<double>(core.freq.value) / 1e9) + "G" : "";
        const std::string tp = core.temp_c > 0
            ? std::to_string(static_cast<int>(core.temp_c + 0.5f)) + "\xc2\xb0" : "";
        const maya::Color tp_c = load_color(std::clamp((core.temp_c - 40.0) / 50.0, 0.0, 1.0));
        // P cores read in the full accent, E cores dimmed — the cluster
        // boundary is legible as COLOR before you read a single letter.
        const maya::Color id_c = !hetero || core.kind == CoreKind::Perf
            ? pal::cpu_ac : mix(pal::cpu_ac, pal::dim, 0.55);
        std::vector<Element> row;
        row.push_back((text(id) | nowrap | Bold | fgc(id_c) | width(id_w)).build());
        row.push_back((Element{Meter{f}.fill().groove(false)} | grow(1)).build());
        if (show_spark)
            row.push_back(Spark{core.history.data(), core.hist_len}.cells(spark_cells).build_fixed());
        row.push_back((text(fmt::pct_pad(f)) | nowrap | fgc(load_color(f))
                       | width(5) | justify(Justify::End)).build());
        if (show_freq_col)
            row.push_back((text(fq) | nowrap | fgc(pal::faint) | width(6) | justify(Justify::End)).build());
        if (show_temp_col)
            row.push_back((text(tp) | nowrap | Bold | fgc(tp_c) | width(5) | justify(Justify::End)).build());
        return (h(std::move(row)) | gap(1)).build();
    };

    // On heterogeneous silicon, group the grid by CLUSTER with its own
    // sub-heading and average load. "Which kind of core is my work landing on"
    // is the question issue #3 asks, and an interleaved flat list answers it
    // only if you decode ids one at a time. Homogeneous machines keep the
    // single flat grid — no wasted heading.
    auto emit_grid = [&](const std::vector<int>& idx) {
        const int m = static_cast<int>(idx.size());
        if (m == 0) return;
        const int gc = std::min(cols, m);
        const int per = (m + gc - 1) / gc;
        for (int r = 0; r < per; ++r) {
            std::vector<Element> cells;
            for (int col = 0; col < gc; ++col) {
                const int k = col * per + r;
                cells.push_back(k < m ? core_row(idx[static_cast<std::size_t>(k)])
                                      : Element{blank()});
            }
            R.push_back(grid_row(std::move(cells), gc, 3));
        }
    };

    if (hetero) {
        std::vector<int> perf, eff;
        for (int i = 0; i < n; ++i) {
            if (c.cores[static_cast<std::size_t>(i)].kind == CoreKind::Eff) eff.push_back(i);
            else perf.push_back(i);
        }
        auto cluster_avg = [&](const std::vector<int>& idx) {
            if (idx.empty()) return 0.0;
            double sum = 0;
            for (int i : idx) sum += c.cores[static_cast<std::size_t>(i)].usage.v;
            return sum / static_cast<double>(idx.size());
        };
        auto heading = [&](const std::string& name, const std::vector<int>& idx, maya::Color ac) {
            const double av = cluster_avg(idx);
            R.push_back((h(
                text("  " + name) | nowrap | Bold | fgc(ac),
                text("  " + std::to_string(idx.size()) + " cores") | nowrap | fgc(pal::faint),
                Element{blank()} | grow(1),
                text("avg " + fmt::pct(av)) | nowrap | Bold | fgc(load_color(av))
            ) | gap(0)).build());
        };
        heading(c.perf_label.empty() ? "PERFORMANCE" : c.perf_label, perf, pal::cpu_ac);
        emit_grid(perf);
        R.push_back(gap_row());
        heading(c.eff_label.empty() ? "EFFICIENCY" : c.eff_label, eff,
                mix(pal::cpu_ac, pal::dim, 0.55));
        emit_grid(eff);
    } else {
        std::vector<int> all(static_cast<std::size_t>(std::max(0, n)));
        for (int i = 0; i < n; ++i) all[static_cast<std::size_t>(i)] = i;
        emit_grid(all);
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
        R.push_back(section("TOP CPU CONSUMERS", pal::cpu_ac, "top " + std::to_string(show) + " · cpu%"));
        for (int i = 0; i < show; ++i) {
            const ProcInfo& p = *top[static_cast<std::size_t>(i)];
            const double f = std::clamp(p.cpu / 100.0, 0.0, 1.0);
            char pct[16]; std::snprintf(pct, sizeof pct, "%5.1f%%", p.cpu);
            R.push_back(rank_row(i + 1, std::to_string(p.pid), maya::truncate_end(p.name, 22),
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
            // Per-core "Core N" temps are already shown inline in the PER-CORE
            // grid — don't repeat them here as a long redundant list.
            if (have_core_temp && sn.zone == "cpu"
                && sn.label.find("ore") != std::string::npos
                && sn.label.find_first_of("0123456789") != std::string::npos)
                continue;
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
            // Width-aware: fixed label(20)+temp(7)+tail(11) columns crush on a
            // thin pane — the label truncates to a stub and the tail clips
            // mid-figure ("high 8"). Shed the tail first, then shrink the
            // label; the meter + temperature always survive.
            const std::string label = sn.label;
            const std::string temp = t;
            const maya::Color tc = load_color(frac);
            R.push_back(Element{maya::ComponentElement{
                .render = [label, tail, temp, frac, tc](int w, int) -> Element {
                    using namespace maya; using namespace maya::dsl;
                    constexpr int kGap = 1, kMeterMin = 4, kTempW = 7;
                    int label_w = 20;
                    bool keep_tail = !tail.empty();
                    auto need = [&] {
                        return label_w + kGap + kMeterMin + kGap + kTempW
                             + (keep_tail ? kGap + 11 : 0);
                    };
                    if (need() > w) keep_tail = false;
                    if (need() > w) label_w = std::max(8, label_w - (need() - w));
                    std::vector<Element> row;
                    row.push_back((text("    " + std::string(truncate_end(label,
                                       std::max(4, label_w - 4))))
                                   | nowrap | fgc(pal::label) | width(label_w)).build());
                    row.push_back((Element{Meter{frac}.fill().groove(false).color(tc)} | grow(1)).build());
                    row.push_back((text(temp) | nowrap | Bold | fgc(tc) | width(kTempW) | justify(Justify::End)).build());
                    if (keep_tail)
                        row.push_back((text(tail) | nowrap | fgc(pal::faint) | width(11) | justify(Justify::End)).build());
                    return (h(std::move(row)) | gap(kGap)).build();
                },
            }});
        }
    }

    if (split) return hero_split(std::move(hero), std::move(left), std::move(right));
    return single;
}

}  // namespace rockbottom::ui::detail
