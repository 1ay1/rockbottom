// ui/kill_plan.hpp — the pure decision logic behind sending a signal.
//
// This is the most safety-critical code in rockbottom: everything else reads
// the machine, this writes to it, and it does so with the user's privileges
// against pids that were named one keystroke ago. It lived inline inside a
// keyboard handler in app.hpp, roughly 800 lines into a std::visit, where it
// could not be tested and could barely be reviewed.
//
// Everything here is a PURE function over a process list. No signals are sent,
// no model is mutated: `plan_*` decides WHICH pids a gesture targets, and
// `resolve_targets` decides which of those are still the processes we armed
// against. The caller (App::update) does the actual kill(2) and owns the toast.
// That split is what makes the race conditions testable — see
// tests/core_logic_test.cpp.
//
// THE RACE THIS EXISTS FOR. Between ARM (user presses x) and CONFIRM (user
// presses y) an arbitrary amount of time passes — the prompt waits forever.
// In that window the target can exit and the kernel can hand its pid to an
// unrelated process. Signalling that new process would be the worst possible
// bug in a system monitor: the tool that is supposed to tell you what is wrong
// with your machine instead kills something at random. So every target carries
// the start_sec it had at arm time, and the confirm path revalidates it.

#pragma once

#include "../core/metrics.hpp"
#include "state.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rockbottom::ui {

// ── pid-reuse guard ─────────────────────────────────────────────────────────

// Does the process now at this pid still look like the one we armed against?
//
// FAILS CLOSED: if either start time is unknown (0) we refuse. This used to
// return true — "unknown, so we can't check" — which inverted the guard
// exactly where it matters, because a pid that has vanished from the snapshot
// is the suspicious case and the gated action is SIGKILL. The costs are not
// symmetric: a false negative means the user presses y again after the next
// sample; a false positive means signalling an unrelated process.
//
// ±2s tolerance: start_sec derives from boot_epoch, which can shift by about a
// second across sampler restarts (the watchdog builds a fresh Sampler).
[[nodiscard]] inline bool start_matches(std::uint64_t armed, std::uint64_t now) noexcept {
    if (armed == 0 || now == 0) return false;   // unknown → refuse
    return armed > now ? armed - now <= 2 : now - armed <= 2;
}

// Map each pid to its start_sec in `procs` (0 when absent). Captured at ARM
// time and compared at CONFIRM time.
[[nodiscard]] inline std::vector<std::uint64_t>
starts_of(const std::vector<ProcInfo>& procs, const std::vector<int>& pids) {
    std::unordered_map<int, std::uint64_t> by_pid;
    by_pid.reserve(procs.size());
    for (const ProcInfo& q : procs) by_pid[q.pid] = q.start_sec;

    std::vector<std::uint64_t> out;
    out.reserve(pids.size());
    for (int pid : pids) {
        auto it = by_pid.find(pid);
        out.push_back(it != by_pid.end() ? it->second : 0);
    }
    return out;
}

// The outcome of revalidating an armed kill against the freshest snapshot:
// which pids may still be signalled, and how many were dropped because the
// process we armed against is gone.
struct ResolvedTargets {
    std::vector<int> signalable;   // still the same process — safe to signal
    int              recycled = 0; // vanished or reborn under the same pid
};

// Split an armed kill's targets into "still safe" and "gone". `starts` is
// index-aligned with `pids` (as PendingKill guarantees); a short or missing
// entry is treated as unknown, which fails closed.
[[nodiscard]] inline ResolvedTargets
resolve_targets(const std::vector<int>& pids,
                const std::vector<std::uint64_t>& starts,
                const std::vector<ProcInfo>& now) {
    std::unordered_map<int, std::uint64_t> now_start;
    now_start.reserve(now.size());
    for (const ProcInfo& q : now) now_start[q.pid] = q.start_sec;

    ResolvedTargets out;
    out.signalable.reserve(pids.size());
    for (std::size_t i = 0; i < pids.size(); ++i) {
        const int pid = pids[i];
        const std::uint64_t armed = i < starts.size() ? starts[i] : 0;
        auto it = now_start.find(pid);
        if (it == now_start.end() || !start_matches(armed, it->second)) {
            ++out.recycled;
            continue;
        }
        out.signalable.push_back(pid);
    }
    return out;
}

// ── target selection ────────────────────────────────────────────────────────

// Every pid sharing a name — the "close all the Chrome helpers" gesture.
// Ordered by pid so the target list is deterministic for a given snapshot
// (the confirm strip shows a count, and a flickering count would be alarming).
[[nodiscard]] inline std::vector<int>
plan_by_name(const std::vector<ProcInfo>& procs, const std::string& name) {
    std::vector<int> pids;
    for (const ProcInfo& q : procs)
        if (q.name == name) pids.push_back(q.pid);
    std::sort(pids.begin(), pids.end());
    return pids;
}

// A process and every descendant — the "reap this whole process group" move.
//
// CYCLE-SAFE. /proc is not a consistent snapshot: it is walked pid by pid
// while the kernel keeps forking and reaping, so a child can be observed
// before its parent is re-parented and the resulting ppid map can contain a
// loop. An unguarded depth-first walk over that map either spins forever or
// returns a target list with duplicates — and this list is fed to kill(2).
// The visited set makes both impossible.
[[nodiscard]] inline std::vector<int>
plan_subtree(const std::vector<ProcInfo>& procs, int root) {
    if (root <= 0) return {};

    std::unordered_map<int, std::vector<int>> kids_of;
    for (const ProcInfo& q : procs)
        if (q.ppid != q.pid)              // a self-parent is a 1-cycle
            kids_of[q.ppid].push_back(q.pid);

    std::vector<int> pids;
    std::unordered_set<int> seen;
    std::vector<int> stack{root};
    seen.insert(root);
    pids.push_back(root);

    while (!stack.empty()) {
        const int cur = stack.back();
        stack.pop_back();
        auto it = kids_of.find(cur);
        if (it == kids_of.end()) continue;
        for (int child : it->second) {
            if (!seen.insert(child).second) continue;   // already queued
            pids.push_back(child);
            stack.push_back(child);
        }
    }
    return pids;
}

// ── result wording ──────────────────────────────────────────────────────────

// What to tell the user after a confirmed kill. Pure so the phrasing is
// testable: the signal-specific verb ("force-killed" vs "asked … to exit") is
// the difference between a message that reads as honest and one that claims
// something the kernel never did.
struct KillOutcome {
    std::string text;
    bool        error = false;
};

[[nodiscard]] inline KillOutcome
describe_outcome(int sig, const std::string& name, int anchor_pid,
                 std::size_t target_count, int ok, int failed, int recycled,
                 const std::string& first_err) {
    if (failed > 0) {
        return {first_err + (target_count > 1 && failed > 1
                                 ? " (+" + std::to_string(failed - 1) + " more failed)"
                                 : ""),
                true};
    }
    if (ok == 0 && recycled > 0)
        return {"target already exited — nothing signaled", true};

    // Signal-aware wording: SIGKILL really did force-kill; SIGTERM only ASKED
    // and the process may still be running when the toast appears. Saying
    // "killed" for a TERM that was ignored would be a lie the UI tells often.
    std::string verb, tail;
    if (sig == SIGKILL)                        { verb = "force-killed "; }
    else if (sig == SIGTERM)                   { verb = "asked ";  tail = " to exit"; }
    else if (sig == SIGSTOP || sig == SIGTSTP) { verb = "suspended "; }
    else if (sig == SIGCONT)                   { verb = "resumed "; }
    else                                       { verb = "sent " + sig_name(sig) + " to "; }

    const std::string what = target_count > 1
        ? std::to_string(ok) + " × " + name
        : name + " (" + std::to_string(anchor_pid) + ")";

    return {verb + what + tail +
                (recycled > 0 ? " (" + std::to_string(recycled) + " already gone)" : ""),
            false};
}

}  // namespace rockbottom::ui
