// verdict.cpp — Translate raw numbers into a plain-language health answer.
// This is the heart of bottom's UX: "what is going on?" resolved to one
// sentence + the culprit, so the UI can lead with the answer.

#include "sampler.hpp"

#include <cstdio>
#include <string>

namespace bottom {

Verdict Sampler::judge(const Snapshot& s) const {
    Verdict v;
    const double cpu      = s.cpu.total.percent();
    const double mem      = s.mem.usage().percent();
    const double swap     = s.mem.swap_usage().percent();
    const double la1      = s.cpu.loadavg[0];
    const double la_ratio = ncpu_ ? la1 / ncpu_ : la1;

    // Loudest CPU and memory consumers among the sampled processes.
    const ProcInfo* top_cpu = nullptr;
    const ProcInfo* top_mem = nullptr;
    for (const auto& p : s.procs) {
        if (!top_cpu || p.cpu > top_cpu->cpu) top_cpu = &p;
        if (!top_mem || p.rss.value > top_mem->rss.value) top_mem = &p;
    }

    auto culprit_cpu = [&]() -> std::string {
        if (top_cpu && top_cpu->cpu > 20)
            return top_cpu->name + " (pid " + std::to_string(top_cpu->pid) + ") is using " +
                   std::to_string(static_cast<int>(top_cpu->cpu)) + "% CPU";
        return {};
    };
    auto culprit_mem = [&]() -> std::string {
        if (top_mem && top_mem->mem_share.percent() > 10)
            return top_mem->name + " holds " + humanize_bytes(top_mem->rss) + " of RAM";
        return {};
    };

    // PSI first: the kernel's own stall accounting beats derived thresholds.
    const double io_stall  = s.psi.io.some_avg10;
    const double mem_stall = s.psi.mem.some_avg10;

    if (io_stall > 40) {
        v.level = io_stall > 70 ? Health::Critical : Health::Stressed;
        v.headline = "Things are waiting on disk I/O";
        char b[64];
        std::snprintf(b, sizeof b, "tasks stalled on I/O %.0f%% of the last 10s", io_stall);
        v.detail = b;
    } else if (mem_stall > 25 || swap > 50 || mem > 92) {
        v.level = (mem_stall > 60 || mem > 95) ? Health::Critical : Health::Stressed;
        v.headline = "Memory is critically tight";
        v.detail = culprit_mem();
    } else if (cpu > 90 || la_ratio > 2.5) {
        v.level = Health::Critical;
        v.headline = "The CPU is maxed out";
        v.detail = culprit_cpu();
    } else if (cpu > 70 || mem > 80 || la_ratio > 1.5) {
        v.level = Health::Stressed;
        if (cpu > 70)  { v.headline = "Working hard — CPU is heavily loaded"; v.detail = culprit_cpu(); }
        else           { v.headline = "Memory is filling up";                 v.detail = culprit_mem(); }
    } else if (cpu > 35 || mem > 60 || (top_cpu && top_cpu->cpu > 40)) {
        v.level = Health::Busy;
        v.headline = "Busy but comfortable";
        v.detail = culprit_cpu();
        if (v.detail.empty()) v.detail = culprit_mem();
    } else {
        v.level = Health::Calm;
        v.headline = "All calm — nothing is straining this machine";
        v.detail = "CPU " + std::to_string(static_cast<int>(cpu)) + "%, RAM " +
                   std::to_string(static_cast<int>(mem)) + "%";
    }

    if (v.detail.empty()) {
        char la_buf[16];
        std::snprintf(la_buf, sizeof la_buf, "%.2f", la1);
        v.detail = "CPU " + std::to_string(static_cast<int>(cpu)) + "%  ·  RAM " +
                   std::to_string(static_cast<int>(mem)) + "%  ·  load " + la_buf;
    }
    return v;
}

}  // namespace bottom
