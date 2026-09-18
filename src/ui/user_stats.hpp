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

    // ── account data, joined from Snapshot::accounts ─────────────────────
    // Present only for users that exist in the passwd database; a process
    // owned by a uid with no account (a deleted user, a container mapping)
    // still gets a row, just without these.
    bool          has_account = false;
    unsigned      uid = 0;
    std::string   home;
    std::string   shell;
    std::string   gecos;
    bool          system = false;
    std::uint64_t disk_bytes = 0;
    std::uint64_t disk_quota = 0;
    bool          disk_known = false;    // false = not measured, NOT "zero"
    bool          disk_partial = false;  // a budgeted scan is still running
    const char*   disk_source = "";
    // Disk usage as a FRACTION, so the roster can rank people against a
    // common scale. Against a quota it's bytes/quota (that's the limit that
    // actually bites); with no quota it's bytes/filesystem-size. 0 when
    // unknown — read disk_known first, since 0 also means "genuinely empty".
    double        disk_share = 0;
    bool          disk_share_of_quota = false;   // which denominator was used
    int           sessions = 0;          // live logins right now

    // ── dashboard aggregates ───────────────────────────────────────
    // Everything below is for the per-user drill-down, not the table. Kept on
    // the same struct so ONE pass over the process list feeds both — the
    // alternative (re-walking procs in the renderer) would run on the paint
    // path for a number the rollup already had in hand.
    std::uint64_t virt = 0;         // summed virtual size
    double        io_read = 0;       // split r/w, because "is this a reader or
    double        io_write = 0;      // a writer" changes what you do about it
    double        faults_ps = 0;     // page faults/sec — memory pressure tell
    double        csw_ps = 0;        // context switches/sec — thrash tell
    int           sleeping = 0;      // S
    int           dstate = 0;        // D — stuck on I/O, the herd that hangs a box
    int           stopped = 0;       // T
    std::uint64_t oldest_start = 0;  // earliest start_sec (0 = unknown)
    int           nice_min = 0;      // scheduling spread: someone running a
    int           nice_max = 0;      // whole build at nice 19 reads differently
    int           fds = 0;           // summed open descriptors (-1s ignored)
    int           port_count = 0;    // distinct listening ports owned
    std::vector<std::uint16_t> ports;      // sorted, deduped — what they expose
    // Per-user CPU history, summed from each process's ring. Gives the
    // dashboard a real sparkline instead of a single instantaneous number.
    std::array<float, 48> cpu_history{};
    int           hist_len = 0;
    // The user's heaviest processes by cpu and by rss, for the dashboard's
    // two top-N lists. pid+name+value only: the Snapshot these came from is
    // replaced every tick and a ProcInfo* would dangle at paint time.
    struct TopProc {
        int pid = 0; std::string name; double cpu = 0; std::uint64_t rss = 0;
        char state = '?'; int threads = 0;
    };
    std::vector<TopProc> heaviest_cpu;   // desc by cpu, capped
    std::vector<TopProc> heaviest_mem;   // desc by rss, capped
    // Live sessions belonging to this user, for the dashboard's session list.
    std::vector<LoginSession> session_list;
};

// Sort key for the users table. Mirrors the process table's idea of "the
// interesting column first" — admins land on this pane asking about load.
enum class UserSort { Cpu, Mem, Procs, Io, Disk, Name };

// Does this user match a roster query? Case-insensitive substring over every
// field an admin might actually remember: the name, the uid, the real name
// (gecos), the home path and the login shell. Deliberately NOT the process
// query language — this is "find the person", and a plain substring is what
// people type for that. An empty query matches everything.
inline bool user_matches(const UserStat& u, const std::string& q) {
    if (q.empty()) return true;
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string needle = lower(q);
    auto has = [&](const std::string& hay) {
        return lower(hay).find(needle) != std::string::npos;
    };
    if (has(u.user) || has(u.gecos) || has(u.home) || has(u.shell)) return true;
    if (u.has_account && has(std::to_string(u.uid))) return true;
    return false;
}

// Apply a roster query to an already-built list, preserving order.
inline std::vector<UserStat> filter_users(const std::vector<UserStat>& us,
                                          const std::string& q) {
    if (q.empty()) return us;
    std::vector<UserStat> out;
    out.reserve(us.size());
    for (const UserStat& u : us)
        if (user_matches(u, q)) out.push_back(u);
    return out;
}

// Size of the filesystem the home directories live on — the denominator for
// "what % of the disk is this user" when no quota exists.
//
// Picked by LONGEST MATCHING MOUNT POINT, the same rule the kernel uses to
// resolve a path to a filesystem: /home is its own mount on many boxes and
// just a directory on / on others, and using the wrong one turns a 40% figure
// into a 4% one. Falls back to / and then to 0 (which renders "—", never a
// made-up percentage).
inline std::uint64_t home_fs_size(const std::vector<DiskInfo>& disks,
                                  const std::vector<UserAccount>& accounts) {
    std::string home;
    for (const UserAccount& a : accounts)
        if (!a.system && !a.home.empty() && a.home != "/") { home = a.home; break; }
    if (home.empty()) home = "/home";

    const DiskInfo* best = nullptr;
    for (const DiskInfo& d : disks) {
        if (d.mount.empty() || d.total.value == 0) continue;
        const bool prefix = home.compare(0, d.mount.size(), d.mount) == 0
            && (d.mount == "/" || home.size() == d.mount.size()
                || home[d.mount.size()] == '/');
        if (!prefix) continue;
        if (!best || d.mount.size() > best->mount.size()) best = &d;
    }
    return best ? best->total.value : 0;
}

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
                                       UserSort sort = UserSort::Cpu,
                                       const std::vector<UserAccount>* accounts = nullptr,
                                       const std::vector<LoginSession>* sessions = nullptr,
                                       bool include_idle_accounts = false,
                                       std::uint64_t home_fs_bytes = 0,
                                       bool desc = true) {
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
        // Dashboard aggregates, same single pass.
        u.virt      += p.virt.value;
        u.io_read   += p.io_read.per_sec;
        u.io_write  += p.io_write.per_sec;
        u.faults_ps += p.faults_ps;
        u.csw_ps    += p.csw_ps;
        if (p.state == 'S') ++u.sleeping;
        if (p.state == 'D') ++u.dstate;
        if (p.state == 'T') ++u.stopped;
        if (p.fds > 0) u.fds += p.fds;
        // Oldest process = roughly "since when has this user been busy".
        if (p.start_sec && (u.oldest_start == 0 || p.start_sec < u.oldest_start))
            u.oldest_start = p.start_sec;
        // Nice spread. Seed both bounds off the FIRST process rather than
        // leaving them at 0, or a user running everything at nice 19 would
        // report a min of 0 they never had.
        if (u.procs == 1) { u.nice_min = u.nice_max = p.nice; }
        else { u.nice_min = std::min(u.nice_min, p.nice); u.nice_max = std::max(u.nice_max, p.nice); }
        u.ports.insert(u.ports.end(), p.ports.begin(), p.ports.end());
        // Sum the per-process CPU rings into one per-user ring.
        //
        // ALIGNMENT IS THE WHOLE DIFFICULTY. push_hist() fills LEFT (index 0
        // first, shifting once full), so a process alive for 3 ticks holds its
        // samples at [0,3) while a long-lived one holds 48 at [0,48) — index i
        // means a DIFFERENT moment in each. Summing index-wise would add a
        // newborn's first sample to an old process's ancient one. So stage
        // right-aligned (newest always in the last slot), which time-aligns
        // newest-to-newest, and un-shift once at the end.
        {
            const int cap = static_cast<int>(p.cpu_history.size());
            const int n = std::clamp(p.hist_len, 0, cap);
            for (int i = 0; i < n; ++i)
                u.cpu_history[static_cast<std::size_t>(cap - n + i)] +=
                    p.cpu_history[static_cast<std::size_t>(i)];
            u.hist_len = std::max(u.hist_len, n);
        }
        u.heaviest_cpu.push_back({p.pid, p.name, p.cpu, p.rss.value, p.state, p.threads});
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

    // Join the account half: identity + disk. `include_idle_accounts` adds a
    // row for a user who owns NO running processes — which is exactly the
    // person filling the disk and then logging out, the case a process-only
    // view can never show.
    if (accounts) {
        for (const UserAccount& a : *accounts) {
            auto it = by_user.find(a.name);
            if (it == by_user.end()) {
                if (!include_idle_accounts) continue;
                if (a.system) continue;          // don't conjure 30 daemon rows
                if (!a.disk_known && !a.can_login) continue;
                UserStat u;
                u.user = a.name;
                it = by_user.emplace(a.name, std::move(u)).first;
            }
            UserStat& u = it->second;
            u.has_account  = true;
            u.uid          = a.uid;
            u.home         = a.home;
            u.shell        = a.shell;
            u.gecos        = a.gecos;
            u.system       = a.system;
            u.disk_bytes   = a.disk_bytes;
            u.disk_quota   = a.disk_quota;
            u.disk_known   = a.disk_known;
            u.disk_partial = a.disk_partial;
            u.disk_source  = a.disk_source;
        }
    }
    if (sessions)
        for (const LoginSession& s : *sessions)
            if (auto it = by_user.find(s.user); it != by_user.end()) {
                ++it->second.sessions;
                it->second.session_list.push_back(s);
            }

    for (auto& [k, v] : by_user) {
        v.mem_share = total_ram ? static_cast<double>(v.rss) / static_cast<double>(total_ram) : 0.0;
        // Ports: dedupe across the user's processes. Two workers of the same
        // server both reporting :443 is one exposed port, not two.
        std::sort(v.ports.begin(), v.ports.end());
        v.ports.erase(std::unique(v.ports.begin(), v.ports.end()), v.ports.end());
        v.port_count = static_cast<int>(v.ports.size());
        // Disk as a fraction. A quota is the denominator that MATTERS when
        // one exists — 48G is fine on a 500G filesystem and an emergency
        // under a 50G cap — so it wins over the filesystem size.
        if (v.disk_known) {
            if (v.disk_quota) {
                v.disk_share = std::clamp(static_cast<double>(v.disk_bytes)
                                        / static_cast<double>(v.disk_quota), 0.0, 1.0);
                v.disk_share_of_quota = true;
            } else if (home_fs_bytes) {
                v.disk_share = std::clamp(static_cast<double>(v.disk_bytes)
                                        / static_cast<double>(home_fs_bytes), 0.0, 1.0);
            }
        }
        // Un-shift the staged right-aligned CPU ring back to push_hist()'s
        // left-aligned convention, so every consumer can read [0, hist_len)
        // oldest→newest like they do for every other ring in the codebase.
        if (v.hist_len > 0 && v.hist_len < static_cast<int>(v.cpu_history.size())) {
            const int cap = static_cast<int>(v.cpu_history.size());
            std::move(v.cpu_history.begin() + (cap - v.hist_len),
                      v.cpu_history.end(), v.cpu_history.begin());
            std::fill(v.cpu_history.begin() + v.hist_len, v.cpu_history.end(), 0.0f);
        }
        // Top-N by cpu and by rss. Built from one collected list rather than
        // two passes; capped at kTopN because the dashboard shows a handful
        // and a user with 400 processes shouldn't cost 400 strings per tick.
        constexpr std::size_t kTopN = 8;
        v.heaviest_mem = v.heaviest_cpu;   // same source, different order
        auto nth = [](std::vector<UserStat::TopProc>& vec, auto cmp) {
            if (vec.size() > kTopN) {
                std::partial_sort(vec.begin(), vec.begin() + kTopN, vec.end(), cmp);
                vec.resize(kTopN);
            } else {
                std::sort(vec.begin(), vec.end(), cmp);
            }
        };
        // Ties break on pid so the lists don't flicker between equal rows.
        nth(v.heaviest_cpu, [](const UserStat::TopProc& a, const UserStat::TopProc& b) {
            return a.cpu != b.cpu ? a.cpu > b.cpu : a.pid < b.pid; });
        nth(v.heaviest_mem, [](const UserStat::TopProc& a, const UserStat::TopProc& b) {
            return a.rss != b.rss ? a.rss > b.rss : a.pid < b.pid; });
        out.push_back(std::move(v));
    }

    // Stable, total ordering: every comparator falls back to the user name so
    // rows never swap places between ticks on equal values (a jittering table
    // is unreadable, and worse, you can select the wrong row).
    auto by_name = [](const UserStat& a, const UserStat& b) { return a.user < b.user; };
    std::sort(out.begin(), out.end(), [&](const UserStat& a, const UserStat& b) {
        // TIERING is NOT reversed by `desc`. It answers "is this row even
        // actionable", not "which is bigger" — flipping it would float the
        // unkillable "?" bucket to row 0 on every ascending sort, which is
        // exactly the state the tiering exists to prevent.
        auto tier = [](const UserStat& u) {
            if (u.user == "?") return 2;
            return u.system ? 1 : 0;
        };
        if (tier(a) != tier(b)) return tier(a) < tier(b);
        // Each arm answers "does a come before b in DESCENDING order?", and
        // `desc` flips that answer once at the end. Doing it per-arm would be
        // six chances to get a comparison backwards; doing it once cannot be
        // inconsistent. Returning early only on INEQUALITY keeps the relation
        // a strict weak ordering — equal values fall through to the name
        // tiebreak rather than reporting both a<b and b<a.
        auto flip = [desc](bool descending_answer) {
            return desc ? descending_answer : !descending_answer;
        };
        switch (sort) {
            case UserSort::Cpu:   if (a.cpu != b.cpu)     return flip(a.cpu > b.cpu);     break;
            case UserSort::Mem:   if (a.rss != b.rss)     return flip(a.rss > b.rss);     break;
            case UserSort::Procs: if (a.procs != b.procs) return flip(a.procs > b.procs); break;
            case UserSort::Io:    if (a.io != b.io)       return flip(a.io > b.io);       break;
            case UserSort::Disk:
                // Rank by SHARE, not raw bytes: 40G of a 50G quota outranks
                // 200G on a 4T array, and that ordering is the whole reason
                // the column exists. Users with no measurement sort last
                // (share 0) rather than interleaving on a figure we don't
                // have. Ties fall through to bytes so two people at 0% still
                // order sensibly.
                if (a.disk_share != b.disk_share) return flip(a.disk_share > b.disk_share);
                if (a.disk_bytes != b.disk_bytes) return flip(a.disk_bytes > b.disk_bytes);
                break;
            case UserSort::Name:
                // Name's "descending" is alphabetical (A→Z), because that's
                // what a reader means by sorting a name column; reversed is
                // Z→A. The generic tiebreak below is always A→Z, so this arm
                // has to handle its own direction rather than fall through.
                if (a.user != b.user) return flip(a.user < b.user);
                break;
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
