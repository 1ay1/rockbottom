// widgets/detail/proc.hpp — the single-PROCESS drill-down body.
//
// More than btop's process popup: identity + full command line, resource bars
// with plain-language read-outs, this process's RANK against every other one
// (so you instantly know if it's THE cpu/mem hog), live disk I/O, thread count,
// a state explanation that tells you why it's stuck, its owner, and every port
// it's listening on. Selection keys still work here so you can walk the list
// without leaving the pane.

#pragma once

#include "common.hpp"

namespace rockbottom::ui::detail {

inline std::vector<Element> proc_body(const Snapshot& s, const Ctx& cx, const ProcInfo* proc) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> b;

    if (!proc) {
        b.push_back(verdict("no process selected — pick one in the table, then press Enter", pal::dim));
        return b;
    }
    const ProcInfo& p = *proc;

    // ── identity ─────────────────────────────────────────────────────────────
    b.push_back((h(
        text("PID " + std::to_string(p.pid)) | nowrap | Bold | fgc(pal::proc_ac) | width(14),
        text(p.name) | nowrap | Bold | fgc(pal::white),
        Element{blank()} | grow(1),
        text(p.user) | nowrap | fgc(pal::label)
    ) | gap(1)).build());
    b.push_back((text("  " + (p.cmd.empty() ? p.name : p.cmd)) | fgc(pal::dim)).build());
    b.push_back(gap_row());

    // ── resources + ranking ──────────────────────────────────────────────────
    b.push_back(section("RESOURCES", pal::proc_ac));
    const double cpuf = std::clamp(p.cpu / 100.0, 0.0, 1.0);
    b.push_back(bar("cpu", cpuf, fmt::fixed1(p.cpu) + "% of one core", load_color(cpuf), cx.wide ? 34 : 0));
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
        "", "", pal::dim));
    if (cpu_rank == 1 && p.cpu > 20)
        b.push_back(verdict("▲ this is the #1 CPU consumer on the machine right now", pal::hot));
    else if (mem_rank == 1 && p.mem_share.v > 0.1)
        b.push_back(verdict("▲ this is the #1 memory consumer on the machine right now", pal::hot));
    b.push_back(gap_row());

    // ── I/O & threads ────────────────────────────────────────────────────────
    b.push_back(section("I/O & THREADS", pal::proc_ac));
    const double ior = p.io_read.per_sec, iow = p.io_write.per_sec;
    b.push_back(kv3(
        "disk read", ior > 1 ? humanize_rate(p.io_read) : "idle", ior > 512 ? pal::teal : pal::dim,
        "disk write", iow > 1 ? humanize_rate(p.io_write) : "idle", iow > 512 ? pal::hot : pal::dim,
        "threads", std::to_string(p.threads), p.threads > 64 ? pal::hot : pal::text));
    b.push_back(gap_row());

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
    b.push_back(kv("owner", p.user, pal::label, 14));

    // ── listening ports ──────────────────────────────────────────────────────
    if (!p.ports.empty()) {
        b.push_back(gap_row());
        b.push_back(section("LISTENING PORTS", pal::proc_ac));
        std::string ports;
        for (std::size_t i = 0; i < p.ports.size() && i < 32; ++i)
            ports += (i ? "  " : "") + (":" + std::to_string(p.ports[i]));
        b.push_back((text("  " + ports) | fgc(pal::sky)).build());
        b.push_back(verdict("this process is accepting connections — it's a server", pal::dim));
    }

    // ── kill hints ───────────────────────────────────────────────────────────
    b.push_back(gap_row());
    b.push_back((h(
        text("  x") | nowrap | Bold | fgc(pal::warn), text("·ask it to stop (SIGTERM)   ") | nowrap | fgc(pal::dim),
        text("K") | nowrap | Bold | fgc(pal::crit), text("·force-kill (SIGKILL)   ") | nowrap | fgc(pal::dim),
        text("↑↓") | nowrap | Bold | fgc(pal::sky), text("·walk the list") | nowrap | fgc(pal::dim)
    )).build());

    return b;
}

}  // namespace rockbottom::ui::detail
