// widgets/detail/proc.hpp — the single-PROCESS drill-down body.
//
// The Process-Hacker pane: identity + full command line + parentage + age,
// resource bars with plain-language read-outs, this process's RANK against
// every other one (so you instantly know if it's THE cpu/mem hog), live disk
// I/O, scheduling facts (priority / nice / threads / open fds / context
// switches), VM behaviour (footprint, fault rate, lifetime pageins), a state
// explanation that tells you why it's stuck, and every port it's listening
// on. Selection keys still work here so you can walk the list without
// leaving the pane.

#pragma once

#include "common.hpp"

#include <cmath>
#include <unordered_map>

namespace rockbottom::ui::detail {

inline std::vector<Element> proc_body(const Snapshot& s, const Ctx& cx, const ProcInfo* proc) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> b;

    if (!proc) {
        b.push_back(verdict("no process selected — pick one in the table, then press Enter", pal::dim));
        return b;
    }
    const ProcInfo& p = *proc;
    const double cpuf = std::clamp(p.cpu / 100.0, 0.0, 1.0);

    // ── CPU hero graph ─────────────────────────────────────────────────────
    // The headline question for a single process is "how much CPU is it
    // eating, and is that spiking or steady?" — so lead with a full-width
    // area graph of this process's own cpu-history ring, the same hero the
    // CPU / GPU panes open with. The ring stores cpu% of ONE core as a 0..1
    // fraction; we render it against a y-axis whose top is the window peak
    // (rounded up to a clean grid) so a task that never breaks 8% still shows
    // real shape instead of a flat line hugging the floor, while a core-pinned
    // hog fills the frame. The live figure rides in the stat card on the left.
    {
        // Peak of the visible window (in cpu% of one core), floored so a near
        // idle process doesn't amplify sampling noise into a full-height wall.
        float ring_peak = 0.0f;
        for (int i = 0; i < p.hist_len && i < 48; ++i)
            ring_peak = std::max(ring_peak, p.cpu_history[static_cast<std::size_t>(i)]);
        const double peak_pct = std::max(static_cast<double>(ring_peak) * 100.0, p.cpu);
        // Clean axis ceiling: 10 / 25 / 50 / 100 / 200 … so labels read round.
        const double axis_top = peak_pct <= 8    ? 10.0
                              : peak_pct <= 20   ? 25.0
                              : peak_pct <= 40   ? 50.0
                              : peak_pct <= 100  ? 100.0
                              : std::ceil(peak_pct / 100.0) * 100.0;
        // Normalize the ring to the axis top so the trace's height matches the
        // percentage the y-axis prints. hero_graph / Graph defer their read to
        // PAINT time (a fill() component resolved against the real slot width),
        // so the sample buffer must OUTLIVE this function — a stack std::array
        // would dangle. Own it in a shared_ptr the render lambda captures.
        auto ring = std::make_shared<std::array<float, 48>>();
        const int hlen = std::min(p.hist_len, 48);
        for (int i = 0; i < hlen; ++i)
            (*ring)[static_cast<std::size_t>(i)] =
                std::clamp(p.cpu_history[static_cast<std::size_t>(i)] * 100.0f
                           / static_cast<float>(axis_top), 0.0f, 1.0f);

        b.push_back(section("CPU OVER TIME", pal::proc_ac,
                            "% of one core · peak " + fmt::fixed1(peak_pct) + "%"));
        // Match the CPU pane's hero height exactly (full cx.graph_h) so the two
        // panes' hero bands are the same size.
        const int gh = cx.graph_h;
        const maya::Color gc = load_color(cpuf);
        // A SECOND owned buffer holding the ring as real cpu%-of-core fractions
        // (0..1, NOT axis-relative), so the stat card's avg / peak / trend
        // arrow are honest — the same genuine trend the CPU pane's card shows,
        // rather than a bogus reading off a [cur, peak] stub.
        auto card_hist = std::make_shared<std::array<float, 48>>();
        for (int i = 0; i < hlen; ++i)
            (*card_hist)[static_cast<std::size_t>(i)] =
                std::clamp(p.cpu_history[static_cast<std::size_t>(i)], 0.0f, 1.0f);
        b.push_back(Element{maya::ComponentElement{
            .render = [ring, card_hist, hlen, gh, gc, cpuf, axis_top]
                      (int w, int) -> Element {
                using namespace maya::dsl;
                std::vector<Element> row;
                // Big-number stat card on the left when there's room — same
                // helper + honest history as the CPU pane's hero.
                if (w >= 64)
                    row.push_back(stat_card(cpuf, gc, "cpu", card_hist->data(), hlen, gh));
                row.push_back(y_axis(gh, axis_top, 4, /*percent=*/true));
                Graph g{ring->data(), hlen};
                g.fill().rows(gh).color(gc);
                row.push_back(Element{g.build()});
                return (h(std::move(row)) | gap(1) | height(gh)).build();
            },
        }});
        b.push_back(gap_row());
    }

    // ── identity ─────────────────────────────────────────────────────────────
    // Name + pid headline; underneath, the full command line and the family
    // line (parent, owner, age) — who spawned it, who owns it, how long it
    // has lived.
    b.push_back((h(
        text("PID " + std::to_string(p.pid)) | nowrap | Bold | fgc(pal::proc_ac) | width(14),
        text(p.name) | nowrap | Bold | fgc(pal::white),
        Element{blank()} | grow(1),
        text(p.state == 'R' ? "● running" : p.state == 'D' ? "◆ blocked"
             : p.state == 'Z' ? "✝ zombie" : p.state == 'T' ? "⏸ stopped" : "○ sleeping")
            | nowrap | Bold
            | fgc(p.state == 'R' ? pal::good : p.state == 'D' ? pal::crit
                  : p.state == 'Z' ? pal::hot : pal::dim)
    ) | gap(1)).build());
    b.push_back((text("  " + (p.cmd.empty() ? p.name : p.cmd)) | fgc(pal::dim)).build());
    {
        // Family strip. Parent gets its OWN full-width row so a long parent
        // name ("tmux: server", "systemd --user") is never clipped into a
        // "tmux: se…" stub the way it was when crammed into a kv3 third;
        // owner + age ride together on the line below.
        std::string parent = p.ppid > 0 ? std::to_string(p.ppid) : "?";
        for (const auto& q : s.procs)
            if (q.pid == p.ppid) { parent += "  " + q.name; break; }
        std::uint64_t now = 0;
        std::string age_txt = "n/a";
        if (p.start_sec > 0) {
            now = static_cast<std::uint64_t>(std::time(nullptr));
            age_txt = now > p.start_sec ? fmt::age(now - p.start_sec) : "just now";
        }
        b.push_back(kv("parent", parent, pal::label, 14));
        b.push_back(kv3(
            "owner", p.user, p.user == "root" ? pal::warn : pal::teal,
            "age", age_txt, pal::text,
            "", "", pal::dim));
    }
    b.push_back(gap_row());

    // ── resources + ranking ──────────────────────────────────────────────────
    b.push_back(section("RESOURCES", pal::proc_ac));
    // p.cpu is % of a SINGLE core (top's convention), so a multithreaded
    // process legitimately exceeds 100% — 312% means it's using 3.1 cores'
    // worth of CPU. A bare "312% of one core" reads like a bug, so when it
    // crosses a core, spell it out in core-equivalents and scale the meter
    // against the WHOLE machine (all cores = full bar) instead of pinning it
    // maxed at 100%.
    const int ncores = std::max(1, s.cpu.logical);
    std::string cpu_read;
    double cpu_meter;
    if (p.cpu > 100.0 && ncores > 1) {
        cpu_read = fmt::fixed1(p.cpu) + "% · " + fmt::fixed1(p.cpu / 100.0) +
                   " of " + std::to_string(ncores) + " cores";
        cpu_meter = std::clamp(p.cpu / (100.0 * ncores), 0.0, 1.0);
    } else {
        cpu_read = fmt::fixed1(p.cpu) + "% of one core";
        cpu_meter = cpuf;
    }
    b.push_back(bar("cpu", cpu_meter, cpu_read, load_color(cpuf), cx.wide ? 34 : 0));
    b.push_back(bar("memory", p.mem_share.v, humanize_bytes(p.rss) + " resident", pal::mem_ac, cx.wide ? 34 : 0));

    // Rank against every process so you know if THIS is the culprit.
    int cpu_rank = 1, mem_rank = 1;
    const int total = static_cast<int>(s.procs.size());
    for (const auto& q : s.procs) {
        if (q.cpu > p.cpu) ++cpu_rank;
        if (q.mem_share.v > p.mem_share.v) ++mem_rank;
    }
    b.push_back(kv3(
        "cpu rank", "#" + std::to_string(cpu_rank) + " of " + std::to_string(total), cpu_rank <= 3 ? pal::hot : pal::dim,
        "mem rank", "#" + std::to_string(mem_rank) + " of " + std::to_string(total), mem_rank <= 3 ? pal::hot : pal::dim,
        "cpu time", p.cpu_ms > 0 ? fmt::cpu_time(p.cpu_ms) : "n/a",
        p.cpu_ms > 3'600'000 ? pal::hot : pal::text));
    if (cpu_rank == 1 && p.cpu > 20)
        b.push_back(verdict("▲ this is the #1 CPU consumer on the machine right now", pal::hot));
    else if (mem_rank == 1 && p.mem_share.v > 0.1)
        b.push_back(verdict("▲ this is the #1 memory consumer on the machine right now", pal::hot));
    b.push_back(gap_row());

    // ── memory detail ────────────────────────────────────────────────────────
    // Footprint is the honest figure (what Activity Monitor's "Memory" column
    // shows): anonymous + compressed + IOKit, not just resident pages.
    if (p.rss.value > 0 || p.virt.value > 0 || p.footprint.value > 0 || p.faults_ps > 0 || p.pageins > 0) {
        b.push_back(section("MEMORY", pal::proc_ac));
        b.push_back(kv3(
            "resident", humanize_bytes(p.rss), pal::mem_ac,
            "virtual", p.virt.value ? humanize_bytes(p.virt) : "n/a", pal::label,
            "footprint", p.footprint.value ? humanize_bytes(p.footprint) : "n/a", pal::mem_ac));
        b.push_back(kv3(
            "of total ram", fmt::pct(p.mem_share.v), pal::label,
            "page faults", fmt::count(p.faults_ps) + "/s",
            p.faults_ps > 10000 ? pal::hot : p.faults_ps > 0.5 ? pal::text : pal::dim,
            "disk pageins", fmt::count(static_cast<double>(p.pageins)),
            pal::label));
        if (p.faults_ps > 50000)
            b.push_back(verdict("▲ faulting very hard — allocating or touching new memory at a furious rate", pal::hot));
        b.push_back(gap_row());
    }

    // ── I/O & scheduling ─────────────────────────────────────────────────────
    b.push_back(section("I/O & SCHEDULING", pal::proc_ac));
    const double ior = p.io_read.per_sec, iow = p.io_write.per_sec;
    b.push_back(kv3(
        "disk read", ior > 1 ? humanize_rate(p.io_read) : "idle", ior > 512 ? pal::teal : pal::dim,
        "disk write", iow > 1 ? humanize_rate(p.io_write) : "idle", iow > 512 ? pal::hot : pal::dim,
        "threads", std::to_string(p.threads), p.threads > 64 ? pal::hot : pal::text));
    b.push_back(kv3(
        "priority", std::to_string(p.prio), pal::label,
        "nice", std::to_string(p.nice), p.nice < 0 ? pal::hot : pal::label,
        "open files", p.fds >= 0 ? std::to_string(p.fds) : "n/a",
        p.fds > 512 ? pal::hot : pal::label));
    if (p.csw_ps > 0.5)
        b.push_back(kv3(
            "ctx switches", fmt::count(p.csw_ps) + "/s",
            p.csw_ps > 20000 ? pal::hot : pal::text,
            "", "", pal::dim,
            "", "", pal::dim));
    if (p.csw_ps > 50000)
        b.push_back(verdict("▲ context-switching very hard — lock contention or chatty IPC", pal::hot));
    b.push_back(gap_row());

    // ── family & children ─────────────────────────────────────────────
    // The whole point of drilling into a process: see and steer its subtree
    // without leaving the pane. Ancestry breadcrumb up top, then the direct
    // children ranked by CPU with the same meter grid the "top" lists use, and
    // a rolled-up subtree total so you know the true cost of the whole family.
    {
        // Index the snapshot by pid for O(1) parent/child walks.
        std::unordered_map<int, const ProcInfo*> by_pid;
        by_pid.reserve(s.procs.size() * 2);
        for (const auto& q : s.procs) by_pid[q.pid] = &q;

        // Direct children, busiest first.
        std::vector<const ProcInfo*> kids;
        for (const auto& q : s.procs)
            if (q.ppid == p.pid && q.pid != p.pid) kids.push_back(&q);
        std::stable_sort(kids.begin(), kids.end(),
                         [](const ProcInfo* a, const ProcInfo* b) { return a->cpu > b->cpu; });

        // Subtree rollup: BFS down from this node summing cpu/mem/count.
        std::unordered_map<int, std::vector<const ProcInfo*>> kids_of;
        for (const auto& q : s.procs)
            if (q.ppid != q.pid) kids_of[q.ppid].push_back(&q);
        int sub_n = 0; double sub_cpu = p.cpu; std::uint64_t sub_rss = p.rss.value;
        {
            std::vector<int> stk{p.pid};
            while (!stk.empty()) {
                int cur = stk.back(); stk.pop_back();
                if (auto it = kids_of.find(cur); it != kids_of.end())
                    for (const ProcInfo* c : it->second) {
                        ++sub_n; sub_cpu += c->cpu; sub_rss += c->rss.value;
                        stk.push_back(c->pid);
                    }
            }
        }

        // Siblings (share this node's parent) — context for "is this one of many?"
        int siblings = 0;
        if (p.ppid > 0)
            for (const auto& q : s.procs)
                if (q.ppid == p.ppid && q.pid != p.pid) ++siblings;

        b.push_back(section("FAMILY", pal::proc_ac,
                            std::to_string(kids.size()) +
                            (kids.size() == 1 ? " child" : " children")));

        // Ancestry breadcrumb: walk ppid up to 4 hops. "launchd › bash › THIS".
        {
            std::vector<std::string> chain;
            const ProcInfo* cur = &p;
            for (int hop = 0; hop < 5 && cur; ++hop) {
                chain.push_back(maya::truncate_end(cur->name, 20) +
                                " (" + std::to_string(cur->pid) + ")");
                auto it = by_pid.find(cur->ppid);
                cur = (cur->ppid > 0 && it != by_pid.end() && it->second != cur)
                          ? it->second : nullptr;
            }
            std::string crumb;
            for (std::size_t i = chain.size(); i-- > 0;)
                crumb += chain[i] + (i ? "  ›  " : "");
            b.push_back(kv("lineage", crumb, pal::label, 14));
        }

        // Subtree rollup line: the true cost of the whole family.
        if (sub_n > 0) {
            b.push_back(kv3(
                "subtree", std::to_string(sub_n + 1) + " procs", pal::text,
                "subtree cpu", fmt::fixed1(sub_cpu) + "%", sub_cpu > 50 ? pal::hot : pal::label,
                "subtree mem", humanize_bytes(sub_rss), pal::mem_ac));
        }
        if (siblings > 0 && sub_n == 0)
            b.push_back(kv("siblings", std::to_string(siblings) + " share this parent",
                           pal::dim, 14));

        // The children themselves, ranked by CPU with the shared meter grid.
        if (!kids.empty()) {
            b.push_back(gap_row());
            // Biggest-child normalizer so the meters are comparable within the
            // family (a 3%-of-core child still shows visible bar shape).
            double kmax = 0.01;
            for (const ProcInfo* c : kids) kmax = std::max(kmax, c->cpu / 100.0);
            const int shown = std::min<int>(static_cast<int>(kids.size()), cx.wide ? 10 : 6);
            for (int i = 0; i < shown; ++i) {
                const ProcInfo* c = kids[static_cast<std::size_t>(i)];
                const double f = std::clamp((c->cpu / 100.0) / kmax, 0.0, 1.0);
                b.push_back(rank_row(
                    i + 1, std::to_string(c->pid), maya::truncate_end(c->name, 22),
                    f, load_color(std::clamp(c->cpu / 100.0, 0.0, 1.0)),
                    fmt::fixed1(c->cpu) + "%", c->cpu > 25 ? pal::hot : pal::label, 8,
                    humanize_bytes(c->rss), pal::mem_ac, 10));
            }
            if (static_cast<int>(kids.size()) > shown)
                b.push_back((text("   +" + std::to_string(static_cast<int>(kids.size()) - shown) +
                                  " more children") | fgc(pal::dim)).build());
        } else {
            b.push_back(verdict("no children — this is a leaf process", pal::dim));
        }
        b.push_back(gap_row());
    }

    // ── state ────────────────────────────────────────────────────────────────
    b.push_back(section("STATE", pal::proc_ac));
    const char* st = p.state == 'R' ? "running — on a core right now"
                   : p.state == 'S' ? "sleeping — parked, waiting for an event"
                   : p.state == 'D' ? "◆ uninterruptible — wedged on I/O, can't even be killed until it returns"
                   : p.state == 'Z' ? "zombie — dead but its parent hasn't reaped it"
                   : p.state == 'T' ? "stopped — suspended (SIGSTOP)"
                   : "unknown";
    const maya::Color sc = p.state == 'R' ? pal::good : p.state == 'D' ? pal::crit
                         : p.state == 'Z' ? pal::hot : pal::dim;
    b.push_back(kv("run state", st, sc, 14));
    if (p.start_sec > 0) {
        char when[64];
        std::time_t t = static_cast<std::time_t>(p.start_sec);
        std::tm tmv{};
        localtime_r(&t, &tmv);
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tmv);
        b.push_back(kv("started", when, pal::label, 14));
    }

    // ── listening ports ──────────────────────────────────────────────────────
    if (!p.ports.empty()) {
        b.push_back(gap_row());
        b.push_back(section("LISTENING PORTS", pal::proc_ac,
                            std::to_string(p.ports.size()) + (p.ports.size() == 1 ? " port" : " ports")));
        std::string ports;
        for (std::size_t i = 0; i < p.ports.size() && i < 32; ++i)
            ports += (i ? "  " : "") + (":" + std::to_string(p.ports[i]));
        b.push_back((text("  " + ports) | fgc(pal::sky)).build());
        b.push_back(verdict("this process is accepting connections — it's a server", pal::dim));
    }

    // ── manage hints ─────────────────────────────────────────────────────
    b.push_back(gap_row());
    b.push_back((h(
        text("  x") | nowrap | Bold | fgc(pal::warn), text("·stop   ") | nowrap | fgc(pal::dim),
        text("K") | nowrap | Bold | fgc(pal::crit), text("·kill   ") | nowrap | fgc(pal::dim),
        text("l") | nowrap | Bold | fgc(pal::hot), text("·signal   ") | nowrap | fgc(pal::dim),
        text("r") | nowrap | Bold | fgc(pal::sky), text("·renice   ") | nowrap | fgc(pal::dim),
        text("X") | nowrap | Bold | fgc(pal::crit), text("·end-all-by-name   ") | nowrap | fgc(pal::dim),
        text("T") | nowrap | Bold | fgc(pal::crit), text("·end whole subtree") | nowrap | fgc(pal::dim)
    )).build());
    b.push_back((h(
        text("  ←→") | nowrap | Bold | fgc(pal::sky), text("·walk to parent / busiest child   ") | nowrap | fgc(pal::dim),
        text("↑↓") | nowrap | Bold | fgc(pal::sky), text("·walk the list") | nowrap | fgc(pal::dim)
    )).build());

    return b;
}

}  // namespace rockbottom::ui::detail
