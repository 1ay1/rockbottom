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
        // Family strip: parent (with its name if it's in the table), owner, age.
        std::string parent = p.ppid > 0 ? std::to_string(p.ppid) : "?";
        for (const auto& q : s.procs)
            if (q.pid == p.ppid) { parent += " " + std::string(fmt::clip(q.name, 18)); break; }
        std::uint64_t now = 0;
        std::string age_txt = "n/a";
        if (p.start_sec > 0) {
            now = static_cast<std::uint64_t>(std::time(nullptr));
            age_txt = now > p.start_sec ? fmt::age(now - p.start_sec) : "just now";
        }
        b.push_back(kv3(
            "parent", parent, pal::label,
            "owner", p.user, p.user == "root" ? mix(pal::hot, pal::label, 0.55) : pal::label,
            "age", age_txt, pal::text));
    }
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
    if (p.footprint.value > 0 || p.faults_ps > 0 || p.pageins > 0) {
        b.push_back(section("MEMORY", pal::proc_ac));
        b.push_back(kv3(
            "resident", humanize_bytes(p.rss), pal::mem_ac,
            "footprint", p.footprint.value ? humanize_bytes(p.footprint) : "n/a", pal::mem_ac,
            "of total ram", fmt::pct(p.mem_share.v), pal::label));
        b.push_back(kv3(
            "page faults", fmt::count(p.faults_ps) + "/s",
            p.faults_ps > 10000 ? pal::hot : p.faults_ps > 0.5 ? pal::text : pal::dim,
            "pageins", fmt::count(static_cast<double>(p.pageins)),
            pal::label,
            "", "", pal::dim));
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
