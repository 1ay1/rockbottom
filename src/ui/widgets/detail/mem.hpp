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

    // ── trend graph — with the live % riding the rule as a gauge pill ──
    {
        std::vector<Element> hdr;
        hdr.push_back(Element{section("USAGE TREND", pal::mem_ac)} | grow(1));
        hdr.push_back((text(" " + fmt::pct(m.usage().v) + " ") | nowrap | Bold
                       | fgc(pal::bg) | bgc(load_color(m.usage().v))).build());
        b.push_back((h(std::move(hdr)) | gap(1)).build());
    }
    {
        const int gh = std::max(4, cx.graph_h - 1);
        std::vector<Element> axis;
        for (int r = 0; r < gh; ++r) {
            std::string lbl = r == 0 ? "100" : r == gh - 1 ? "  0" : r == gh / 2 ? " 50" : "   ";
            axis.push_back((text(lbl) | nowrap | fgc(pal::faint)).build());
        }
        b.push_back((h(
            v(std::move(axis)) | width(3),
            Element{Graph{m.usage_history.data(), m.hist_len}.fill().rows(gh).color(pal::mem_ac)} | grow(1)
        ) | gap(1) | height(gh)).build());
    }
    b.push_back(gap_row());

    // ── physical breakdown ───────────────────────────────────────────────────
    // "used" from /proc includes nothing reclaimable; apps + cache + buffers +
    // free = total. We separate them so you see what's actually committed.
    b.push_back(section("PHYSICAL", pal::mem_ac));
    b.push_back(bar("used", m.usage().v, humanize_bytes(m.used) + " / " + humanize_bytes(m.total), pal::mem_ac, cx.wide ? 34 : 0));
    b.push_back(bar("cache", Ratio::of(m.cached, m.total).v, humanize_bytes(m.cached) + " reclaimable", pal::teal, cx.wide ? 34 : 0));
    b.push_back(bar("buffers", Ratio::of(m.buffers, m.total).v, humanize_bytes(m.buffers), pal::sky, cx.wide ? 34 : 0));

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

    // ── swap ─────────────────────────────────────────────────────────────────
    b.push_back(section("SWAP", pal::mem_ac));
    if (m.swap_total.value > 0) {
        b.push_back(bar("used", m.swap_usage().v,
                        humanize_bytes(m.swap_used) + " / " + humanize_bytes(m.swap_total), pal::hot, cx.wide ? 34 : 0));
        b.push_back(kv3(
            "paging in", humanize_rate(m.swap_in), m.swap_in.per_sec > 1024 ? pal::crit : pal::dim,
            "paging out", humanize_rate(m.swap_out), m.swap_out.per_sec > 1024 ? pal::crit : pal::dim,
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
        const int show = std::min<int>(cx.tall ? 6 : 4, static_cast<int>(top.size()));
        for (int i = 0; i < show; ++i) {
            const ProcInfo& p = *top[static_cast<std::size_t>(i)];
            b.push_back((h(
                text(std::to_string(p.pid)) | nowrap | fgc(pal::dim) | width(8),
                text(fmt::clip(p.name, 22)) | nowrap | fgc(pal::text) | width(23),
                Element{Meter{p.mem_share.v}.fill().groove(false).color(pal::mem_ac)} | grow(1),
                text(fmt::pct_pad(p.mem_share.v)) | nowrap | Bold | fgc(pal::mem_ac) | width(6) | justify(Justify::End),
                text(humanize_bytes(p.rss)) | nowrap | fgc(pal::label) | width(10) | justify(Justify::End)
            ) | gap(1)).build());
        }
    }

    return b;
}

}  // namespace rockbottom::ui::detail
