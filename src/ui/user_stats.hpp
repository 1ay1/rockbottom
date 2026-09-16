// ui/user_stats.hpp — per-user resource rollup: the admin's "who is eating
// this box" answer, as a PURE function over the process list.
//
// A system monitor shows you processes; an admin thinks in PEOPLE. On a shared
// box — a build farm, a jump host, a university server, a CI runner — the
// question is rarely "which pid is hot" but "which USER is hot", and the
// follow-up is "show me their processes" or "stop all of it". Answering that
// today means piping ps through awk. This makes it a keystroke.
//
// Kept as a pure function (no UI types, no model mutation) for the same reason
// kill_plan.hpp is: the aggregation feeds a KILL gesture, so it has to be
// testable in isolation. See tests/core_logic_test.cpp.

#pragma once

#include "../core/metrics.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rockbottom::ui {

// One row of the USERS table: a person and everything they're running.
struct UserStat {
    std::string   user;
    int           procs = 0;        // processes owned
    int           threads = 0;      // threads across those processes
    double        cpu = 0;          // summed cpu% (0..100*ncores)
    std::uint64_t rss = 0;          // summed resident bytes
    double        mem_share = 0;    // rss / total ram (0..1)
    double        io = 0;           // summed read+write bytes/sec
    int           running = 0;      // procs in R state
    int           zombies = 0;      // procs in Z state — a user's orphaned mess
    // The single biggest process this user owns, for the "top" column. Held as
    // a pid + name rather than a pointer: the snapshot this was computed from
    // is replaced every tick, and a dangling ProcInfo* would be read at paint
    // time (the same lifetime trap traffic_hero documents).
    int           top_pid = 0;
    std::string   top_name;
    double        top_cpu = 0;
};

// Sort key for the users table. Mirrors the process table's idea of "the
// interesting column first" — admins land on this pane asking about load.
enum class UserSort { Cpu, Mem, Procs, Io, Name };

// Roll the process list up by owner.
//
// `total_ram` is used for mem_share; 0 disables it (share stays 0) rather than
// dividing by zero. Processes with an empty user string are bucketed under
// "?" instead of being dropped — an unknown owner is itself worth seeing, and
// silently discarding rows would make the totals disagree with the process
// pane, which is exactly the kind of inconsistency that erodes trust in a
// monitor.
inline std::vector<UserStat> user_stats(const std::vector<ProcInfo>& procs,
                                        std::uint64_t total_ram,
                                        UserSort sort = UserSort::Cpu) {
    std::unordered_map<std::string, UserStat> by_user;
    by_user.reserve(32);

    for (const ProcInfo& p : procs) {
        UserStat& u = by_user[p.user.empty() ? std::string("?") : p.user];
        if (u.user.empty()) u.user = p.user.empty() ? "?" : p.user;
        ++u.procs;
        u.threads += std::max(0, p.threads);
        u.cpu     += p.cpu;
        u.rss     += p.rss.value;
        u.io      += p.io_read.per_sec + p.io_write.per_sec;
        if (p.state == 'R') ++u.running;
        if (p.state == 'Z') ++u.zombies;
        // "Biggest" means CPU — the column an admin is scanning when they
        // open this pane. The `top_pid == 0` arm matters more than it looks:
        // without it a user whose processes are ALL idle never claims the
        // slot (0 > 0 is false), so a user owning 292 sleeping daemons would
        // render "—" as their busiest process. Ties resolve to the lower pid
        // so the row doesn't flicker between two idle processes every tick.
        if (u.top_pid == 0 || p.cpu > u.top_cpu
            || (p.cpu == u.top_cpu && p.pid < u.top_pid)) {
            u.top_cpu = p.cpu;
            u.top_pid = p.pid;
            u.top_name = p.name;
        }
    }

    std::vector<UserStat> out;
    out.reserve(by_user.size());
    for (auto& [k, v] : by_user) {
        v.mem_share = total_ram ? static_cast<double>(v.rss) / static_cast<double>(total_ram) : 0.0;
        out.push_back(std::move(v));
    }

    // Stable, total ordering: every comparator falls back to the user name so
    // rows never swap places between ticks on equal values (a jittering table
    // is unreadable, and worse, you can select the wrong row).
    auto by_name = [](const UserStat& a, const UserStat& b) { return a.user < b.user; };
    std::sort(out.begin(), out.end(), [&](const UserStat& a, const UserStat& b) {
        switch (sort) {
            case UserSort::Cpu:   if (a.cpu != b.cpu)     return a.cpu > b.cpu;     break;
            case UserSort::Mem:   if (a.rss != b.rss)     return a.rss > b.rss;     break;
            case UserSort::Procs: if (a.procs != b.procs) return a.procs > b.procs; break;
            case UserSort::Io:    if (a.io != b.io)       return a.io > b.io;       break;
            case UserSort::Name:  break;
        }
        return by_name(a, b);
    });
    return out;
}

// Every pid owned by `user`, for the kill-all-by-user gesture.
//
// SAFETY: this list goes to kill(2), so it is deliberately conservative.
//   * Exact user match, never substring — "adm" must not select "admin".
//   * pid <= 1 is refused outright. Signalling init/launchd is never what an
//     admin means by "log this user out", and pid 0 would fan the signal to
//     our own process group (see signal_process in sampler.cpp).
//   * `self` (our own pid) is excluded: killing the monitor mid-sweep would
//     leave the job half-done with no UI to see what happened. Pass 0 to
//     disable that filter.
// The caller still runs the start_sec pid-reuse revalidation in kill_plan.hpp
// on confirm; this only decides the candidate set.
inline std::vector<int> plan_by_user(const std::vector<ProcInfo>& procs,
                                     const std::string& user, int self = 0) {
    std::vector<int> pids;
    if (user.empty() || user == "?") return pids;   // "unknown owner" is not a target
    for (const ProcInfo& p : procs) {
        if (p.user != user) continue;
        if (p.pid <= 1) continue;
        if (self && p.pid == self) continue;
        pids.push_back(p.pid);
    }
    std::sort(pids.begin(), pids.end());
    pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    return pids;
}

}  // namespace rockbottom::ui
