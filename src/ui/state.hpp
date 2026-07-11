// state.hpp — small UI-interaction state types shared between the App
// program and the widgets that render them.

#pragma once

#include "../core/metrics.hpp"
#include "../core/sampler.hpp"   // SortKey

#include <csignal>
#include <string>
#include <vector>

namespace rockbottom {

// A kill awaiting confirmation. `pids` may carry a whole GROUP (kill-all-
// by-name); `pid` stays the anchor process for messaging either way.
struct PendingKill {
    int pid = 0;
    std::string name;
    int sig = SIGTERM;
    std::vector<int> pids;   // every target; size()>1 = group kill
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
    std::vector<const ProcInfo*> procs;   // filtered, sorted
    SortKey sort = SortKey::Cpu;
    int selected = 0;
    int max_rows = 10;
    int width = 120;                      // panel inner width, for column tiers
    std::string filter;
    bool filtering = false;
    const PendingKill* pending = nullptr;
};

}  // namespace ui
}  // namespace rockbottom
