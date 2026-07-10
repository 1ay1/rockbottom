// widgets/detail/mem.hpp — the MEMORY drill-down body.
//
// More than btop's segmented bar: a usage trend graph, a physical breakdown
// that separates what apps actually hold from reclaimable cache/buffers, the
// available-memory verdict that tells you whether you're actually low, full
// swap accounting with a thrash detector, PSI memory pressure, and the top
// memory-hungry processes right here so you don't have to go hunting.

#pragma once

#include "common.hpp"

namespace rockbottom::ui::detail {

inline std::vector<Element> mem_body(const Snapshot& s, const Ctx& cx) {
    using namespace maya; using namespace maya::dsl;
    const MemInfo& m = s.mem;
    std::vector<Element> b;

    // ── hero: BIG number + trend graph ──────────────────────────────────
    b.push_back(section("USAGE TREND", pal::mem_ac));
    {
        const int gh = std::max(4, cx.graph_h - 1);
        std::vector<Element> axis;
        for (int r = 0; r < gh; ++r) {
            std::string lbl = r == 0 ? "100" : r == gh - 1 ? "  0" : r == gh / 2 ? " 50" : "   ";
            axis.push_back((text(lbl) | nowrap | fgc(pal::faint)).build());
        }
        b.push_back((h(
            stat_card(m.usage().v, pal::mem_ac, "ram used",
                      m.usage_history.data(), m.hist_len, gh),
            v(std::move(axis)) | width(3),
            Element{Graph{m.usage_history.data(), m.hist_len}.fill().rows(gh).color(pal::mem_ac)} | grow(1)
        ) | gap(1) | height(gh)).build());
    }
    b.push_back(gap_row());

    // ── physical composition ─────────────────────────────────────────────
    // ONE stacked bar shows how RAM is divided (the Activity-Monitor idiom)
    // — apps / wired / compressed / cache, free as the quiet tail — then a
    // legend with the exact figures. Far more legible than five separate
    // meters that all start at the same left edge.
    b.push_back(section("PHYSICAL", pal::mem_ac));
    {
        const double t = static_cast<double>(m.total.value ? m.total.value : 1);
        std::vector<Seg> segs;
        std::vector<LegendItem> leg;
        std::uint64_t seg_sum = 0;
        auto add = [&](Bytes v, const char* name, maya::Color c) {
            if (!v.value) return;
            segs.push_back({static_cast<double>(v.value) / t, c});
            leg.push_back({name, humanize_bytes(v), c});
            seg_sum += v.value;
        };
        if (m.app.value || m.wired.value || m.compressed.value) {
            add(m.app, "apps", pal::mem_ac);
            add(m.wired, "wired", pal::hot);
            add(m.compressed, "compressed", pal::pink);
            add(m.cached, "cache", pal::teal);
            add(m.buffers, "purgeable", pal::sky);
        } else {
            // Linux shape: used / cache / buffers.
            add(m.used, "used", pal::mem_ac);
            add(m.cached, "cache", pal::teal);
            add(m.buffers, "buffers", pal::sky);
        }
        // The trailing figure and the "free" swatch must agree with what the
        // bar PAINTS (sum of segments), not with the abstract used counter —
        // a bar at 75% captioned "3.8G / 16G" reads as a bug.
        const std::uint64_t painted = std::min(seg_sum, m.total.value);
        const std::uint64_t tail = m.total.value > painted ? m.total.value - painted : 0;
        b.push_back((h(
            text("ram") | nowrap | fgc(pal::label) | width(14),
            Element{comp_bar(std::move(segs))} | grow(1),
            text(humanize_bytes(Bytes{painted}) + " / " + humanize_bytes(m.total))
                | nowrap | Bold | fgc(pal::label) | width(16) | justify(Justify::End)
        ) | gap(2)).build());
        leg.push_back({"free", humanize_bytes(Bytes{tail}),
                       mix(pal::dim, pal::bg_panel, 0.5)});
        b.push_back((h(
            Element{blank()} | width(16),
            Element{comp_legend(leg)} | grow(1)
        )).build());
    }

    // Available is the number that actually matters — how much a new process
    // could take without swapping. Read it out plainly.
    const double avail_f = Ratio::of(m.available, m.total).v;
    const char* av = avail_f > 0.4 ? "● healthy — plenty free for new work"
                   : avail_f > 0.2 ? "● getting tight — cache would shrink before you swap"
                   : avail_f > 0.1 ? "▲ low — new allocations may start pushing to swap"
                   :                 "▲ critical — the machine is nearly out of RAM";
    const maya::Color avc = avail_f > 0.4 ? pal::good : avail_f > 0.2 ? pal::teal
                          : avail_f > 0.1 ? pal::hot : pal::crit;
    b.push_back(kv3(
        "available", humanize_bytes(m.available), avc,
        "of total", fmt::pct(avail_f), avc,
        "total ram", humanize_bytes(m.total), pal::text));
    b.push_back(verdict(av, avc));
    b.push_back(gap_row());

    // ── VM activity ───────────────────────────────────────────────────
    // How hard the paging machinery is working right now — fault rate plus
    // file-backed page traffic. High pagein = cold starts / cache misses.
    if (m.faults_ps > 0 || m.page_in.per_sec > 0 || m.page_out.per_sec > 0) {
        b.push_back(section("VM ACTIVITY", pal::mem_ac));
        b.push_back(kv3(
            "page faults", fmt::count(m.faults_ps) + "/s",
            m.faults_ps > 50000 ? pal::hot : pal::text,
            "page in", humanize_rate(m.page_in),
            m.page_in.per_sec > 1 << 20 ? pal::hot : pal::label,
            "page out", humanize_rate(m.page_out),
            m.page_out.per_sec > 1 << 20 ? pal::hot : pal::label));
        b.push_back(gap_row());
    }

    // ── swap ─────────────────────────────────────────────────────────────────
    b.push_back(section("SWAP", pal::mem_ac));
    if (m.swap_total.value > 0) {
        b.push_back(bar("used", m.swap_usage().v,
                        humanize_bytes(m.swap_used) + " / " + humanize_bytes(m.swap_total), pal::hot, cx.wide ? 34 : 0));
        b.push_back(kv3(
            "swapping in", humanize_rate(m.swap_in), m.swap_in.per_sec > 1024 ? pal::crit : pal::dim,
            "swapping out", humanize_rate(m.swap_out), m.swap_out.per_sec > 1024 ? pal::crit : pal::dim,
            "", "", pal::dim));
        const double churn = m.swap_in.per_sec + m.swap_out.per_sec;
        b.push_back(verdict(
            churn > 4096 ? "▲ actively thrashing — the machine wants more RAM than it has"
          : churn > 256  ? "● light paging — occasional, nothing alarming"
          :                "● swap is parked; nothing moving in or out",
            churn > 4096 ? pal::crit : churn > 256 ? pal::hot : pal::good));
    } else {
        b.push_back(verdict("no swap configured — when RAM runs out, the OOM killer decides who dies", pal::dim));
    }
    b.push_back(gap_row());

    // ── PSI memory pressure ──────────────────────────────────────────────────
    if (s.psi.mem.available) {
        b.push_back(section("PRESSURE · PSI last 10s", pal::mem_ac));
        b.push_back(bar("some stalled", s.psi.mem.some_avg10 / 100.0, "≥1 task waited on memory", pal::hot, cx.wide ? 34 : 0));
        b.push_back(bar("full stalled", s.psi.mem.full_avg10 / 100.0, "everything waited at once", pal::crit, cx.wide ? 34 : 0));
        b.push_back(gap_row());
    }

    // ── top memory consumers ─────────────────────────────────────────────────
    // Bring the answer to "what's eating my RAM?" right into the pane.
    {
        std::vector<const ProcInfo*> top;
        for (const auto& p : s.procs) top.push_back(&p);
        std::sort(top.begin(), top.end(),
                  [](const ProcInfo* a, const ProcInfo* b2) { return a->mem_share.v > b2->mem_share.v; });
        b.push_back(section("TOP MEMORY CONSUMERS", pal::mem_ac));
        const int show = std::min<int>(cx.tall ? 8 : 4, static_cast<int>(top.size()));
        for (int i = 0; i < show; ++i) {
            const ProcInfo& p = *top[static_cast<std::size_t>(i)];
            b.push_back(rank_row(i + 1, std::to_string(p.pid), std::string(fmt::clip(p.name, 22)),
                                 p.mem_share.v, pal::mem_ac,
                                 fmt::pct_pad(p.mem_share.v), pal::mem_ac, 6,
                                 humanize_bytes(p.rss), pal::label, 10));
        }
    }

    return b;
}

}  // namespace rockbottom::ui::detail
