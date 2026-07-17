// state.hpp — small UI-interaction state types shared between the App
// program and the widgets that render them.

#pragma once

#include "../core/metrics.hpp"
#include "../core/sampler.hpp"   // SortKey

#include <csignal>
#include <cstdint>
#include <string>
#include <vector>

namespace rockbottom {

// A kill awaiting confirmation. `pids` may carry a whole GROUP (kill-all-
// by-name); `pid` stays the anchor process for messaging either way.
// `starts` is index-aligned with `pids`: each target's start_sec captured at
// ARM time. The confirm can come arbitrarily later, and a pid recycled in
// between must not be signaled — the confirm path revalidates each target's
// start time against the freshest snapshot and skips mismatches. 0 = unknown
// (start unavailable), which skips the check for that pid only.
struct PendingKill {
    int pid = 0;
    std::string name;
    int sig = SIGTERM;
    std::vector<int> pids;   // every target; size()>1 = group kill
    std::vector<std::uint64_t> starts;   // start_sec per target (pid-reuse guard)
};

// One entry in the signal picker: the number, its POSIX name, and a one-word
// gloss of what it does. Ordered the way htop's F9 menu lists them — the
// everyday ones first, the exotic ones after.
struct SignalDef {
    int         num;
    const char* name;   // "SIGTERM"
    const char* gloss;  // "graceful stop"
};

// The catalog the picker renders. Kept deliberately curated (not every signal
// in <csignal>) so the menu stays a glanceable page, not a syscall reference.
inline const std::vector<SignalDef>& signal_catalog() {
    static const std::vector<SignalDef> kSignals = {
        {SIGTERM, "SIGTERM", "graceful stop"},
        {SIGKILL, "SIGKILL", "force kill"},
        {SIGINT,  "SIGINT",  "interrupt (^C)"},
        {SIGHUP,  "SIGHUP",  "hang up / reload"},
        {SIGQUIT, "SIGQUIT", "quit + core dump"},
        {SIGSTOP, "SIGSTOP", "suspend"},
        {SIGCONT, "SIGCONT", "resume"},
        {SIGUSR1, "SIGUSR1", "user-defined 1"},
        {SIGUSR2, "SIGUSR2", "user-defined 2"},
        {SIGABRT, "SIGABRT", "abort"},
        {SIGWINCH,"SIGWINCH","window resize"},
        {SIGTSTP, "SIGTSTP", "stop (^Z)"},
    };
    return kSignals;
}

// Human name for a signal number ("SIGTERM"), for the confirm strip / toast.
// Falls back to "signal N" for anything outside the curated catalog.
inline std::string sig_name(int sig) {
    for (const auto& s : signal_catalog())
        if (s.num == sig) return s.name;
    return "signal " + std::to_string(sig);
}

// Transient notification shown in the footer for a few ticks.
struct Toast {
    std::string text;
    bool error = false;
    int ttl = 4;   // ticks
};

namespace ui {

// Everything the process panel needs to render one frame.
struct ProcView {
    std::vector<const ProcInfo*> procs;   // filtered, ordered (sorted OR tree)
    SortKey sort = SortKey::Cpu;
    bool sort_desc = true;                // ▼ high-to-low (default) vs ▲ low-to-high
    int selected = 0;
    int scroll = 0;                       // first visible row (sticky window top)
    int max_rows = 10;
    int width = 120;                      // panel inner width, for column tiers
    std::string filter;
    bool filtering = false;
    const PendingKill* pending = nullptr;

    // Tree mode: when true, `procs` is in parent→child order and `tree_prefix`
    // carries the ASCII guide ("│  ├─ ") for each row, index-aligned to `procs`.
    // A row whose subtree is collapsed gets a ▸ marker in `collapsed_row`.
    bool tree = false;
    std::vector<std::string> tree_prefix;
    std::vector<bool> has_kids;       // row heads a subtree (can be collapsed)
    std::vector<bool> collapsed_row;  // row's subtree is currently collapsed
    std::vector<int>  hidden_count;   // descendants folded under a collapsed row
    std::vector<bool> context_row;    // kept only as ancestor of a match → dim
    std::vector<double> sub_cpu;      // subtree CPU% rollup (self + descendants)
    std::vector<double> sub_mem;      // subtree RSS-bytes rollup
    std::vector<double> sib_share;    // flow tree: cpu share vs busiest sibling (0..1)
    int follow_pid = 0;               // PID locked with * (badge in the chip)
};

}  // namespace ui
}  // namespace rockbottom
