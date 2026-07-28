// widgets/detail/cpu.hpp — the CPU drill-down body.
//
// More than btop's per-core strip: a hero load graph with a real y-axis, a
// distribution strip (min / median / max / spread across cores), a saturation
// verdict that compares load-average to core count, package temperature, and
// a PER-CORE TABLE — one logical core per row, under one column header, in the
// pane's most visible position (the whole left column when the screen is wide
// enough to split).

#pragma once

#include "common.hpp"

namespace rockbottom::ui::detail {

// ── PER-CORE TABLE ──────────────────────────────────────────────────────
// ONE logical core per row: id · load bar · trend spark · load% · clock · °C.
//
// LAYOUT CONTRACT. Every cell in a row is FIXED-WIDTH, and the widths come
// from core_cols() against the row's REAL slot width — the same function the
// header row calls. Two consequences, both load-bearing:
//
//   * the header and every data row agree at ANY terminal size, because they
//     solved the same arithmetic rather than both trusting flex;
//   * no cell can be flex-shrunk by a long sibling, so a core with no
//     temperature probe can't slide its neighbours' numbers left (issue #2 —
//     the widths are a property of the TABLE, never of the row's contents).
//
// Columns are shed cheapest-information-first as the slot narrows (°C, then
// clock, then the trend spark, then the bar itself), so the table degrades to
// a legible "id + %" list on a phone-width terminal instead of overflowing.
struct CoreTableCfg {
    int  id_w = 3;        // widest id string in the table ("ALL", "127", "12·E")
    bool freq = false;    // any core reports a clock
    bool temp = false;    // any core reports a die temperature
};

struct CoreCols {
    int gap = 1;
    int id_w = 3, meter_w = 0, spark_w = 0, pct_w = 5, freq_w = 0, temp_w = 0;
};

inline CoreCols core_cols(int w, const CoreTableCfg& cfg) {
    CoreCols c;
    c.id_w   = std::max(3, cfg.id_w);
    c.pct_w  = 5;                      // "100%" + a cell of air
    c.freq_w = cfg.freq ? 5 : 0;       // "3.90" (header carries the unit)
    c.temp_w = cfg.temp ? 4 : 0;       // "100"  (header carries the unit)
    const int W = std::max(1, w);

    // Total row width if the bar took `meter` cells and the spark `spark`.
    // Counts the inter-cell gaps for the cells actually PRESENT, so the
    // arithmetic stays exact as columns drop out.
    auto span = [&](int meter, int spark) {
        int cells = 2, sum = c.id_w + c.pct_w;      // id + % never drop
        if (meter > 0)  { ++cells; sum += meter; }
        if (spark > 0)  { ++cells; sum += spark; }
        if (c.freq_w)   { ++cells; sum += c.freq_w; }
        if (c.temp_w)   { ++cells; sum += c.temp_w; }
        return sum + c.gap * (cells - 1);
    };

    constexpr int kMeterMin = 6;   // below this a bar is decoration, not data
    if (span(kMeterMin, 0) > W && c.temp_w) c.temp_w = 0;
    if (span(kMeterMin, 0) > W && c.freq_w) c.freq_w = 0;

    // Whatever is left belongs to the bar; the trend takes a slice of it once
    // the row is roomy enough for both to say something. The bar stays the
    // DOMINANT cell — it's the column you scan vertically — and the spark caps
    // at 40 because CpuCore::history only holds 48 samples; a wider spark would
    // just be stretched air.
    const int room = W - span(0, 0) - c.gap;
    if (room < 3) return c;            // no bar at all at this width
    if (room >= 34) {
        c.spark_w = std::clamp(room * 2 / 5, 10, 40);
        c.meter_w = room - c.spark_w - c.gap;
    } else {
        c.meter_w = room;
    }
    return c;
}

// One table row's data, resolved to strings/colors before render so the
// component lambda owns everything it paints (no dangling spans into a
// snapshot that may have been resampled by paint time).
struct CoreRow {
    std::string        id;
    maya::Color        id_c = pal::cpu_ac;
    double             load = 0;
    std::vector<float> hist;
    std::string        freq, temp;
    maya::Color        temp_c = pal::dim;
    bool               anchor = false;   // the ALL row: brighter ink, no dimming
};

inline Element core_row_el(CoreRow d, CoreTableCfg cfg) {
    using namespace maya;
    return Element{maya::ComponentElement{
        .render = [d, cfg](int w, int) -> Element {
            using namespace maya; using namespace maya::dsl;
            const CoreCols cc = core_cols(w, cfg);
            const maya::Color lc = load_color(d.load);
            std::vector<Element> row;
            row.push_back((text(d.id) | nowrap | Bold | fgc(d.id_c)
                           | width(cc.id_w)).build());
            if (cc.meter_w > 0) {
                // Groove ON: the empty remainder is a recessed slab, so the
                // bars form one continuous track down the table and two cores
                // are comparable by eye without reading either number.
                row.push_back((Element{Meter{d.load}.width(cc.meter_w).color(lc)}
                               | width(cc.meter_w)).build());
            }
            if (cc.spark_w > 0) {
                Element sp = d.hist.empty()
                    ? Element{blank()}
                    : Spark{d.hist.data(), static_cast<int>(d.hist.size())}
                          .cells(cc.spark_w).build_fixed();
                row.push_back((std::move(sp) | width(cc.spark_w)).build());
            }
            row.push_back((text(fmt::pct_pad(d.load)) | nowrap | Bold | fgc(lc)
                           | width(cc.pct_w) | justify(Justify::End)).build());
            if (cc.freq_w)
                row.push_back((text(d.freq) | nowrap
                               | fgc(d.anchor ? pal::label : pal::dim)
                               | width(cc.freq_w) | justify(Justify::End)).build());
            if (cc.temp_w)
                row.push_back((text(d.temp) | nowrap | Bold | fgc(d.temp_c)
                               | width(cc.temp_w) | justify(Justify::End)).build());
            return (h(std::move(row)) | gap(cc.gap)).build();
        },
    }};
}

// The column header — same widths, same order, so it labels what is actually
// underneath it at every width (and disappears column by column with it).
inline Element core_head_row(CoreTableCfg cfg) {
    using namespace maya;
    return Element{maya::ComponentElement{
        .render = [cfg](int w, int) -> Element {
            using namespace maya; using namespace maya::dsl;
            const CoreCols cc = core_cols(w, cfg);
            auto cell = [](const char* t, int cw, bool right) {
                Element e = (text(cw >= static_cast<int>(std::string(t).size()) ? t : "")
                             | nowrap | fgc(pal::faint) | width(cw)
                             | justify(right ? Justify::End : Justify::Start)).build();
                return e;
            };
            std::vector<Element> row;
            row.push_back(cell(cc.id_w >= 4 ? "CORE" : "CPU", cc.id_w, false));
            if (cc.meter_w > 0) row.push_back(cell("LOAD", cc.meter_w, false));
            if (cc.spark_w > 0) row.push_back(cell("TREND", cc.spark_w, false));
            row.push_back(cell("USE", cc.pct_w, true));
            if (cc.freq_w) row.push_back(cell("GHz", cc.freq_w, true));
            if (cc.temp_w) row.push_back(cell("\xc2\xb0" "C", cc.temp_w, true));
            return (h(std::move(row)) | gap(cc.gap)).build();
        },
    }};
}

// Returns the body rows for the CPU pane (unframed, unscrolled).
inline std::vector<Element> cpu_body(const Snapshot& s, const Ctx& cx) {
    using namespace maya; using namespace maya::dsl;
    const CpuInfo& c = s.cpu;

    // READING ORDER. The per-core table is the thing you open this pane FOR,
    // so it gets the most visible real estate: the LEFT column when the screen
    // is wide enough to split, and the slot directly under the hero graph when
    // it isn't. RIGHT NOW + DISTRIBUTION — the at-a-glance interpretation of
    // that table — sit DIRECTLY BELOW it (they read as the table's summary, not
    // a parallel column). Top consumers + sensors ride the stat column / below.
    //
    // Four collectors, assembled at the bottom so the same section code serves
    // both layouts and neither can drift from the other.
    std::vector<Element> hero, core_col, below_core, stat_col;
    const bool split = cx.ultrawide;
    std::vector<Element>& B = below_core; // right-now + distribution, under the table
    std::vector<Element>& L = stat_col;   // top consumers, sensors
    std::vector<Element>& R = core_col;   // the per-core table

    // ── hero: BIG number + graph ───────────────────────────────
    // Grafana stat-panel idiom: the headline figure in block digits with a
    // trend arrow, parked left of the load graph — readable across the room.
    // In split mode the hero rides the FULL pane width (hero_split) so the
    // trace spans the whole first band instead of a half-width sliver.
    std::vector<Element>& H = hero;
    H.push_back(section("LOAD OVER TIME", pal::cpu_ac));
    {
        const int gh = cx.graph_h;
        // CPU load is the filled mountain; RAM usage rides ON TOP as a second
        // line (same idiom the MEM/GPU heroes use for their overlay), so this
        // one graph answers "is the box busy, and is it also full?" at a glance.
        H.push_back(hero_graph(c.total.v, load_color(c.total.v), "cpu load",
                               c.total_history.data(), c.total_hist_len, gh,
                               std::nullopt,
                               s.mem.usage_history.data(), s.mem.hist_len, pal::mem_ac));
        // Tiny legend so the two traces are unambiguous: the fill is CPU, the
        // line is RAM — with both live figures right there.
        H.push_back((h(
            text("\xe2\x96\x88 cpu ") | nowrap | fgc(load_color(c.total.v)),
            text(fmt::pct(c.total.v)) | nowrap | Bold | fgc(load_color(c.total.v)),
            text("    \xe2\x94\x80 ram ") | nowrap | fgc(pal::mem_ac),
            text(fmt::pct(s.mem.usage().v)) | nowrap | Bold | fgc(pal::mem_ac),
            Element{blank()} | grow(1),
            text(std::string(humanize_bytes(s.mem.used)) + " / " +
                 std::string(humanize_bytes(s.mem.total))) | nowrap | fgc(pal::dim)
        ) | gap(0)).build());
    }
    if (!split) H.push_back(gap_row());

    // ── right-now stat strip (sits DIRECTLY UNDER the per-core table) ──────
    B.push_back(gap_row());
    B.push_back(section("RIGHT NOW", pal::cpu_ac));
    B.push_back(bar("total", c.total.v, "busy across all cores", load_color(c.total.v), cx.wide ? 34 : 0));
    // User/system split — the first question about a busy CPU: is it MY code
    // or the kernel? Heavy system time usually means syscall/IO churn.
    if (c.user.v > 0 || c.system.v > 0) {
        B.push_back(bar("user", c.user.v, "running app code", pal::cpu_ac, cx.wide ? 34 : 0));
        B.push_back(bar("system", c.system.v, "in the kernel (syscalls)", pal::hot, cx.wide ? 34 : 0));
    }
    if (c.iowait.v > 0.005)
        B.push_back(bar("iowait", c.iowait.v, "stalled waiting on disk", pal::hot, cx.wide ? 34 : 0));

    // Memory, right here beside the CPU load — the two numbers you check
    // together ("is it CPU-bound or is it running out of RAM?"). Colored on
    // its own ramp so a full box reads hot even while the CPU sits idle.
    {
        const double mu = s.mem.usage().v;
        B.push_back(bar("memory", mu,
            std::string(humanize_bytes(s.mem.used)) + " of " +
                std::string(humanize_bytes(s.mem.total)) + " used",
            load_color(mu), cx.wide ? 34 : 0));
        if (s.mem.swap_total.value > 0 && s.mem.swap_usage().v > 0.01)
            B.push_back(bar("swap", s.mem.swap_usage().v,
                std::string(humanize_bytes(s.mem.swap_used)) + " swapped out",
                s.mem.swap_usage().v > 0.5 ? pal::crit : pal::hot, cx.wide ? 34 : 0));
    }

    // Load average, interpreted against the core count — the number htop shows
    // but never explains. >1.0 per core = the run queue is backing up.
    const int lc = std::max(1, c.logical);
    const double sat = c.loadavg[0] / lc;
    const char* verdict_txt =
        sat < 0.7 ? "\xe2\x97\x8f plenty of headroom \xe2\x80\x94 nothing is queuing for a core"
      : sat < 1.0 ? "\xe2\x97\x8f comfortably busy \xe2\x80\x94 cores keeping up with demand"
      : sat < 2.0 ? "\xe2\x96\xb2 oversubscribed \xe2\x80\x94 tasks are waiting for a free core"
      :             "\xe2\x96\xb2 heavily saturated \xe2\x80\x94 the run queue is deep, things will feel slow";
    const maya::Color vc = sat < 0.7 ? pal::good : sat < 1.0 ? pal::teal
                         : sat < 2.0 ? pal::hot : pal::crit;
    B.push_back(kv3(
        "load 1m", fmt::fixed2(c.loadavg[0]), load_color(std::min(1.0, sat)),
        "5m", fmt::fixed2(c.loadavg[1]), pal::label,
        "15m", fmt::fixed2(c.loadavg[2]), pal::label));
    // Core topology reads "8 (4P + 4E)" on heterogeneous silicon.
    std::string topo = std::to_string(c.logical);
    if (c.hetero())
        topo += " (" + std::to_string(c.perf_cores) + "P + " + std::to_string(c.eff_cores) + "E)";
    B.push_back(kv3(
        "logical cpus", topo, pal::text,
        "load / core", fmt::fixed2(sat), vc,
        c.temp_c > 1 ? "package" : "", c.temp_c > 1 ? std::to_string(static_cast<int>(c.temp_c)) + " \xc2\xb0" "C" : "",
        load_color(std::clamp((c.temp_c - 40) / 50.0, 0.0, 1.0))));
    B.push_back(verdict(verdict_txt, vc));
    B.push_back(gap_row());

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
        B.push_back(section("DISTRIBUTION", pal::cpu_ac));
        B.push_back(kv3(
            "busiest core", fmt::pct(hi), load_color(hi),
            "quietest", fmt::pct(lo), load_color(lo),
            "median", fmt::pct(med), load_color(med)));
        B.push_back(kv3(
            "average", fmt::pct(avg), load_color(avg),
            "spread", fmt::pct(hi - lo), hi - lo > 0.5 ? pal::hot : pal::dim,
            "active cores", std::to_string(active) + "/" + std::to_string(n),
            active > n / 2 ? pal::hot : pal::good));
        if (hi - lo > 0.6 && hi > 0.8)
            B.push_back(verdict("\xe2\x96\xb2 load is lopsided \xe2\x80\x94 one core is pinned while others idle "
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
            B.push_back(kv3(
                "P cores", fmt::pct(pavg) + " avg", load_color(pavg),
                "E cores", fmt::pct(eavg) + " avg", load_color(eavg),
                "headroom", pn ? fmt::pct(1.0 - pavg) + " on P" : "",
                1.0 - pavg > 0.5 ? pal::good : pal::hot));
            if (eavg > 0.7 && pavg < 0.3)
                B.push_back(verdict("\xe2\x96\xb2 the efficiency cores are doing the work while the "
                                    "performance cores idle \xe2\x80\x94 pin the hot process to a P core",
                                    pal::hot));
        }
        B.push_back(gap_row());
    }

    // ── PER-CORE TABLE ──────────────────────────────────────────────────
    // ONE logical core per row, under ONE header, in the pane's primary slot.
    //
    // Why not the old N-column grid: with 3-4 columns you had to re-find the
    // number rails for every column, the row you wanted sat in an arbitrary
    // one of them (column-major fill), and each core's cells were squeezed to a
    // third of the width — so the trend spark and the °C column got dropped on
    // exactly the wide machines that most need them. One core per row is
    // scannable top-to-bottom, every core carries its FULL data (bar, trend,
    // load, clock, temperature), and the bars form one continuous track you can
    // read as a profile of the whole package without reading a single digit.
    //
    // The class and the temperature arrive RESOLVED from the sampler
    // (CpuCore::kind / ::temp_c); this pane never parses a sensor label or
    // guesses a cluster layout from core counts.
    const int n = static_cast<int>(c.cores.size());
    const bool hetero = c.hetero();

    bool have_core_temp = false;
    for (const CpuCore& core : c.cores) if (core.temp_c > 0) { have_core_temp = true; break; }
    bool have_freq = false;
    for (const CpuCore& core : c.cores) if (core.freq.value > 0) { have_freq = true; break; }

    // The id column is sized from the WIDEST id the table can contain, not
    // from a constant: a 4-thread laptop spends 3 cells, a 256-thread server
    // spends 5, and a hybrid part adds the "·P"/"·E" tag. Nothing here can be
    // outgrown by a bigger machine.
    const int id_digits = std::max<int>(2, static_cast<int>(
        std::to_string(std::max(0, n - 1)).size()));
    CoreTableCfg cfg;
    cfg.id_w = std::max(3, hetero ? id_digits + 2 : id_digits);
    cfg.freq = have_freq;
    cfg.temp = have_core_temp;

    {
        std::string chip = std::to_string(n) + (n == 1 ? " thread" : " threads");
        if (hetero)
            chip = std::to_string(c.perf_cores) + "P + " + std::to_string(c.eff_cores)
                 + "E · " + chip;
        R.push_back(section("PER-CORE", pal::cpu_ac, chip));
    }
    R.push_back(core_head_row(cfg));

    // Package row FIRST, on the same rails as the cores below it: "is this core
    // above or below the machine as a whole" becomes a straight horizontal
    // comparison instead of mental arithmetic against a number in another
    // section.
    {
        CoreRow all;
        all.id = "ALL";
        all.id_c = pal::white;
        all.load = c.total.v;
        all.anchor = true;
        all.hist.assign(c.total_history.data(),
                        c.total_history.data() + std::clamp(c.total_hist_len, 0,
                            static_cast<int>(c.total_history.size())));
        if (have_freq) {
            double sum = 0; int k = 0;
            for (const CpuCore& core : c.cores)
                if (core.freq.value > 0) { sum += static_cast<double>(core.freq.value); ++k; }
            if (k) all.freq = fmt::fixed2(sum / k / 1e9);
        }
        if (have_core_temp) {
            // The HOTTEST core, not the package sensor. This row heads a column
            // of per-core die temperatures, and on plenty of machines the
            // package/"Tctl" sensor reads far below them — printing it here
            // would make the summary row contradict every row beneath it.
            float t = 0;
            for (const CpuCore& core : c.cores) t = std::max(t, core.temp_c);
            if (t > 1) {
                all.temp = std::to_string(static_cast<int>(t + 0.5f));
                all.temp_c = load_color(std::clamp((t - 40.0) / 50.0, 0.0, 1.0));
            }
        }
        R.push_back(core_row_el(std::move(all), cfg));
    }
    // One core → one row. Everything is resolved to plain strings/colors here;
    // core_row_el owns the copy, so a row can be laid out (and re-laid out at a
    // new width) without reaching back into the snapshot.
    auto core_row = [&](int i) -> Element {
        const CpuCore& core = c.cores[static_cast<std::size_t>(i)];
        CoreRow d;
        char id[16];
        if (hetero)
            std::snprintf(id, sizeof id, "%*d\xc2\xb7%c", id_digits, i,
                          core.kind == CoreKind::Eff ? 'E' : 'P');
        else
            std::snprintf(id, sizeof id, "%*d", id_digits, i);
        d.id = id;
        // P cores read in the full accent, E cores dimmed — the cluster
        // boundary is legible as COLOR before you read a single letter.
        d.id_c = !hetero || core.kind == CoreKind::Perf
            ? pal::cpu_ac : mix(pal::cpu_ac, pal::dim, 0.55);
        d.load = core.usage.v;
        d.hist.assign(core.history.data(),
                      core.history.data() + std::clamp(core.hist_len, 0,
                          static_cast<int>(core.history.size())));
        if (core.freq.value > 0)
            d.freq = fmt::fixed2(static_cast<double>(core.freq.value) / 1e9);
        if (core.temp_c > 0) {
            d.temp = std::to_string(static_cast<int>(core.temp_c + 0.5f));
            d.temp_c = load_color(std::clamp((core.temp_c - 40.0) / 50.0, 0.0, 1.0));
        }
        return core_row_el(std::move(d), cfg);
    };

    // On heterogeneous silicon, group the table by CLUSTER with its own
    // sub-heading and average load. "Which kind of core is my work landing on"
    // is the question issue #3 asks, and an interleaved flat list answers it
    // only if you decode ids one at a time. Homogeneous machines keep the
    // single flat table — no wasted heading.
    auto emit_rows = [&](const std::vector<int>& idx) {
        for (int i : idx) R.push_back(core_row(i));
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
            R.push_back(gap_row());
            R.push_back((h(
                text(name) | nowrap | Bold | fgc(ac),
                text("  " + std::to_string(idx.size()) + " cores") | nowrap | fgc(pal::faint),
                Element{blank()} | grow(1),
                text("avg " + fmt::pct(av)) | nowrap | Bold | fgc(load_color(av))
            ) | gap(0)).build());
        };
        heading(c.perf_label.empty() ? "PERFORMANCE" : c.perf_label, perf, pal::cpu_ac);
        emit_rows(perf);
        heading(c.eff_label.empty() ? "EFFICIENCY" : c.eff_label, eff,
                mix(pal::cpu_ac, pal::dim, 0.55));
        emit_rows(eff);
    } else {
        std::vector<int> all(static_cast<std::size_t>(std::max(0, n)));
        for (int i = 0; i < n; ++i) all[static_cast<std::size_t>(i)] = i;
        emit_rows(all);
    }
    R.push_back(gap_row());

    // ── top CPU consumers ───────────────────────────────────────────
    // The question a hot CPU pane exists to answer: WHO. Same ranked-list
    // grid as the memory / disk panes. Lives with the INTERPRETATION column,
    // beside the per-core table — "core 7 is pinned" and "this is the process
    // pinning it" read together instead of a screen apart.
    {
        std::vector<const ProcInfo*> top;
        for (const auto& p : s.procs) top.push_back(&p);
        std::sort(top.begin(), top.end(),
                  [](const ProcInfo* a, const ProcInfo* b2) { return a->cpu > b2->cpu; });
        // The table column grows with the core count, so on a hybrid/server box
        // the stat column has vertical room to spare — spend it on a deeper
        // top-N rather than leaving the band blank.
        const int cap = split ? std::clamp(n, 8, 16) : cx.tall ? 8 : 4;
        const int show = std::min<int>(cap, static_cast<int>(top.size()));
        L.push_back(gap_row());
        L.push_back(section("TOP CPU CONSUMERS", pal::cpu_ac, "top " + std::to_string(show) + " · cpu%"));
        for (int i = 0; i < show; ++i) {
            const ProcInfo& p = *top[static_cast<std::size_t>(i)];
            const double f = std::clamp(p.cpu / 100.0, 0.0, 1.0);
            char pct[16]; std::snprintf(pct, sizeof pct, "%5.1f%%", p.cpu);
            L.push_back(rank_row(i + 1, std::to_string(p.pid), maya::truncate_end(p.name, 22),
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
        L.push_back(gap_row());
        L.push_back(section("SENSORS", pal::cpu_ac,
                            std::to_string(s.sensors.size()) + " probes"));
        std::string cur_zone = "\x01";   // sentinel so the first row prints its zone
        for (const Sensor& sn : s.sensors) {
            // Per-core "Core N" temps already have their own °C column in the
            // PER-CORE table — don't repeat them here as a long redundant list.
            if (have_core_temp && sn.zone == "cpu"
                && sn.label.find("ore") != std::string::npos
                && sn.label.find_first_of("0123456789") != std::string::npos)
                continue;
            if (sn.zone != cur_zone) {
                cur_zone = sn.zone;
                L.push_back((text("  " + cur_zone) | nowrap | fgc(pal::faint)).build());
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
            L.push_back(Element{maya::ComponentElement{
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

    // ── ASSEMBLY ───────────────────────────────────────────────────────
    // The per-core TABLE leads (it's what the eye lands on), and RIGHT NOW +
    // DISTRIBUTION sit DIRECTLY BENEATH it — they read as the table's summary.
    // Wide: hero across the top, [table + summary] as the left column, top
    // consumers + sensors as the right. Narrow: one column in the same order.
    if (split) {
        // In the two-column layout, placing the summary at the bottom of the
        // (tall) left column hides it below ~12 core rows. Render it FULL-WIDTH
        // beneath the split band instead, so it sits visibly under the table.
        auto out = hero_split(std::move(hero), std::move(core_col), std::move(stat_col));
        out.push_back(gap_row());
        out.insert(out.end(), std::make_move_iterator(below_core.begin()),
                              std::make_move_iterator(below_core.end()));
        return out;
    }
    core_col.insert(core_col.end(),
                    std::make_move_iterator(below_core.begin()),
                    std::make_move_iterator(below_core.end()));
    std::vector<Element> out = std::move(hero);
    out.insert(out.end(), std::make_move_iterator(core_col.begin()),
                          std::make_move_iterator(core_col.end()));
    out.insert(out.end(), std::make_move_iterator(stat_col.begin()),
                          std::make_move_iterator(stat_col.end()));
    return out;
}

}  // namespace rockbottom::ui::detail
