// state.hpp — small UI-interaction state types shared between the App
// program and the widgets that render them.

#pragma once

#include "../core/metrics.hpp"
#include "../core/sampler.hpp"   // SortKey

#include <csignal>
#include <string>
#include <vector>

namespace rockbottom {

// A kill awaiting confirmation.
struct PendingKill {
    int pid = 0;
    std::string name;
    int sig = SIGTERM;
};

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
