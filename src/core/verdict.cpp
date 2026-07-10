// verdict.cpp — The diagnostic engine. bottom's soul.
//
// This is not a threshold ladder; it's a differential diagnosis. Each tick we
// collect FINDINGS (independent observations of distress, each with severity
// and plain-language evidence), then the strongest finding becomes the
// headline and the runners-up become the detail. The result reads like a
// good SRE glancing at the box, not like a gauge crossing a line.
//
// Signals used, in order of trustworthiness:
//   PSI            kernel's own stall accounting — ground truth for "waiting"
//   vmstat paging  live swap in/out — actual thrashing vs parked swap
//   iowait split   cores idle *because* of disk
//   per-core skew  one pinned core = single-threaded bottleneck, hidden in avg
//   temp           thermal throttling risk
//   run queue      loadavg vs cores + R-count — queued work vs busy work
//   D/Z states     stuck-on-io herds, zombie leaks
//   trends         CPU history slope + RAM history slope (leak suspicion)
//   OOM proximity  MemAvailable in absolute terms, not percentage

#include "sampler.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace bottom {
namespace {

struct Finding {
    int         score = 0;        // 0-100 severity; >=70 critical, >=45 stressed, >=25 busy
    std::string headline;         // plain sentence, no jargon required to parse
    std::string evidence;         // the receipts: numbers + culprit
};

std::string fmt_pct(double v)   { char b[16]; std::snprintf(b, sizeof b, "%.0f%%", v); return b; }
std::string fmt_1(double v)     { char b[16]; std::snprintf(b, sizeof b, "%.1f", v);   return b; }

// Least-squares slope of a history ring, in units-per-sample. Cheap, robust
// enough for "is this line going up".
double slope(const float* d, int n) {
    if (n < 8) return 0;
    double sx = 0, sy = 0, sxy = 0, sxx = 0;
    for (int i = 0; i < n; ++i) {
        sx += i; sy += d[i]; sxy += static_cast<double>(i) * d[i]; sxx += static_cast<double>(i) * i;
    }
    double denom = n * sxx - sx * sx;
    return denom == 0 ? 0 : (n * sxy - sx * sy) / denom;
}

}  // namespace

Verdict Sampler::judge(const Snapshot& s) const {
    const double cpu       = s.cpu.total.percent();
    const double iowait    = s.cpu.iowait.percent();
    const double mem       = s.mem.usage().percent();
    const double avail_gb  = static_cast<double>(s.mem.available.value) / (1024.0 * 1024 * 1024);
    const double la1       = s.cpu.loadavg[0];
    const double la5       = s.cpu.loadavg[1];
    const double la15      = s.cpu.loadavg[2];
    const double la_ratio  = ncpu_ ? la1 / ncpu_ : la1;
    const double swap_traffic = s.mem.swap_in.per_sec + s.mem.swap_out.per_sec;   // B/s
    const double io_stall  = s.psi.io.available  ? s.psi.io.some_avg10  : 0;
    const double mem_stall = s.psi.mem.available ? s.psi.mem.some_avg10 : 0;
    const double cpu_stall = s.psi.cpu.available ? s.psi.cpu.some_avg10 : 0;

    // ── Culprits ────────────────────────────────────────────────────────────
    const ProcInfo* top_cpu = nullptr;
    const ProcInfo* top_mem = nullptr;
    const ProcInfo* top_d   = nullptr;   // a D-state process to name for io findings
    for (const auto& p : s.procs) {
        if (!top_cpu || p.cpu > top_cpu->cpu) top_cpu = &p;
        if (!top_mem || p.rss.value > top_mem->rss.value) top_mem = &p;
        if (p.state == 'D' && (!top_d || p.cpu > top_d->cpu)) top_d = &p;
    }
    auto name_pid = [](const ProcInfo* p) {
        return p ? p->name + " (pid " + std::to_string(p->pid) + ")" : std::string{};
    };

    // Concentration: how much of the total CPU burn is ONE process?
    double procs_cpu_sum = 0;
    for (const auto& p : s.procs) procs_cpu_sum += p.cpu;
    const double cpu_share = (top_cpu && procs_cpu_sum > 1)
                                 ? top_cpu->cpu / procs_cpu_sum : 0;

    // Per-core skew: max core vs average — the single-thread signature.
    double max_core = 0;
    for (const auto& c : s.cpu.cores) max_core = std::max(max_core, c.usage.percent());
    const double avg_core = cpu;
    const bool one_core_pinned = max_core > 92 && avg_core < 55 && ncpu_ > 2;

    // Trends over ~2 minutes of samples.
    const double cpu_slope = slope(s.cpu.total_history.data(), s.cpu.total_hist_len) * 100;
    const double mem_slope = slope(s.mem.usage_history.data(), s.mem.hist_len) * 100;
    const bool mem_leaking = mem_slope > 0.08 && s.mem.hist_len > 60 && mem > 50;
    const bool cpu_rising  = cpu_slope > 0.25 && s.cpu.total_hist_len > 30;

    std::vector<Finding> findings;

    // ── 1. Thrashing: live paging traffic is the emergency, not swap % ──────
    if (swap_traffic > 5.0 * 1024 * 1024) {
        findings.push_back({90,
            "The machine is thrashing — RAM is overcommitted",
            "paging " + humanize_rate(ByteRate{swap_traffic}) + " to swap; " +
                name_pid(top_mem) + " holds " + humanize_bytes(top_mem ? top_mem->rss : Bytes{})});
    } else if (swap_traffic > 256.0 * 1024) {
        findings.push_back({55,
            "Memory pressure — actively swapping",
            "paging " + humanize_rate(ByteRate{swap_traffic}) +
                "; available RAM down to " + humanize_bytes(s.mem.available)});
    }

    // ── 2. OOM proximity: absolute available matters more than percent ──────
    if (avail_gb < 0.35 && s.mem.total.value > (2ull << 30)) {
        findings.push_back({95,
            "Nearly out of memory — OOM killer is close",
            "only " + humanize_bytes(s.mem.available) + " available; biggest holder is " +
                name_pid(top_mem) + " at " + humanize_bytes(top_mem ? top_mem->rss : Bytes{})});
    } else if (mem > 90 || mem_stall > 20) {
        findings.push_back({60,
            "Memory is very tight",
            (mem_stall > 20
                 ? "tasks stalled on memory " + fmt_pct(mem_stall) + " of the last 10s; "
                 : humanize_bytes(s.mem.available) + " available; ") + name_pid(top_mem) +
                " holds " + humanize_bytes(top_mem ? top_mem->rss : Bytes{})});
    }

    // ── 3. I/O-bound: PSI + iowait + D-states, with the stuck process named ─
    if (io_stall > 30 || iowait > 30 || s.dstate >= 3) {
        int sev = io_stall > 60 ? 80 : io_stall > 30 ? 60 : 45;
        std::string ev;
        if (io_stall > 0)  ev += "tasks stalled on I/O " + fmt_pct(io_stall) + " of the last 10s";
        if (iowait > 15)   ev += (ev.empty() ? "" : "; ") + ("cores idle-waiting " + fmt_pct(iowait));
        if (s.dstate > 0)  ev += (ev.empty() ? "" : "; ") + std::to_string(s.dstate) +
                                 " uninterruptible" + (top_d ? " incl. " + name_pid(top_d) : "");
        findings.push_back({sev, "Disk I/O is the bottleneck", std::move(ev)});
    }

    // ── 4. CPU saturated: distinguish one-hog vs many-way contention ────────
    if (cpu > 92) {
        std::string ev;
        if (cpu_share > 0.6 && top_cpu) {
            ev = name_pid(top_cpu) + " alone is burning " + fmt_1(top_cpu->cpu) + "% \u2014 " +
                 "kill it and the machine is fine";
        } else {
            ev = std::to_string(s.running) + " processes runnable across " +
                 std::to_string(ncpu_) + " cores; top: " + name_pid(top_cpu) +
                 " at " + fmt_1(top_cpu ? top_cpu->cpu : 0) + "%";
        }
        findings.push_back({cpu_stall > 25 ? 78 : 70, "The CPU is saturated", std::move(ev)});
    } else if (cpu > 70) {
        findings.push_back({48, "CPU is working hard",
            name_pid(top_cpu) + " leads at " + fmt_1(top_cpu ? top_cpu->cpu : 0) + "%" +
                (cpu_rising ? "; load has been climbing" : "")});
    }

    // ── 5. Single-thread bottleneck: invisible in the average ───────────────
    if (one_core_pinned && top_cpu && top_cpu->cpu > 80) {
        findings.push_back({40,
            "Single-threaded bottleneck — one core pinned",
            name_pid(top_cpu) + " is maxing one core (" + fmt_pct(max_core) +
                ") while the machine averages " + fmt_pct(avg_core)});
    }

    // ── 6. Run-queue backlog: work queued beyond capacity ───────────────────
    if (la_ratio > 2.0 && cpu < 80) {
        // High load + moderate CPU = queue is full of something not running
        // (usually D-state I/O), a classically confusing state for users.
        findings.push_back({50,
            "Work is queuing up faster than it can run",
            "load " + fmt_1(la1) + " on " + std::to_string(ncpu_) + " cores (" +
                fmt_1(la_ratio) + "x capacity)" +
                (s.dstate > 0 ? "; " + std::to_string(s.dstate) + " tasks stuck in I/O" : "")});
    } else if (la_ratio > 1.3) {
        findings.push_back({35, "Load exceeds core count",
            "load " + fmt_1(la1) + " / " + fmt_1(la5) + " / " + fmt_1(la15) + " on " +
                std::to_string(ncpu_) + " cores" +
                (la1 > la15 * 1.4 ? " and rising" : la1 < la15 * 0.7 ? " but falling" : "")});
    }

    // ── 7. Thermal ───────────────────────────────────────────────────────────
    if (s.cpu.temp_c > 92) {
        findings.push_back({72, "The CPU is overheating",
            fmt_1(s.cpu.temp_c) + "°C — expect thermal throttling" +
                (top_cpu && top_cpu->cpu > 50 ? "; heat source: " + name_pid(top_cpu) : "")});
    } else if (s.cpu.temp_c > 82) {
        findings.push_back({38, "Running hot",
            fmt_1(s.cpu.temp_c) + "°C under " + fmt_pct(cpu) + " CPU load"});
    }

    // ── 8. Memory leak suspicion: steady climb, no obvious spike ────────────
    if (mem_leaking) {
        findings.push_back({42, "Memory use is climbing steadily",
            "RAM grew ~" + fmt_pct(mem_slope * 60) + "/min for the last few minutes; " +
                "watch " + name_pid(top_mem) + " (" +
                humanize_bytes(top_mem ? top_mem->rss : Bytes{}) + ")"});
    }

    // ── 9. Zombie herd: cosmetic but worth a mention ────────────────────────
    if (s.zombies >= 10) {
        findings.push_back({30, "Zombie processes are accumulating",
            std::to_string(s.zombies) + " zombies — a parent isn't reaping its children"});
    }

    // ── Synthesis: strongest finding wins; runner-up rides in the detail ────
    Verdict v;
    if (findings.empty()) {
        v.level = Health::Calm;
        // Even calm is informative: say WHY it's calm and what the busiest thing is.
        v.headline = "All calm — nothing is straining this machine";
        std::string busiest;
        if (top_cpu && top_cpu->cpu >= 3)
            busiest = "; busiest: " + top_cpu->name + " at " + fmt_1(top_cpu->cpu) + "%";
        v.detail = "CPU " + fmt_pct(cpu) + " · RAM " + fmt_pct(mem) + " (" +
                   humanize_bytes(s.mem.available) + " free) · load " + fmt_1(la1) + busiest;
        return v;
    }

    std::sort(findings.begin(), findings.end(),
              [](const Finding& a, const Finding& b) { return a.score > b.score; });
    const Finding& top = findings.front();

    v.level = top.score >= 70 ? Health::Critical
            : top.score >= 45 ? Health::Stressed
            : Health::Busy;
    v.headline = top.headline;
    v.detail   = top.evidence;

    // Second opinion: if a distinct runner-up is also significant, append it.
    if (findings.size() > 1 && findings[1].score >= 35 &&
        findings[1].headline != top.headline) {
        v.detail += "  ·  also: " + findings[1].headline;
    }
    return v;
}

}  // namespace bottom
