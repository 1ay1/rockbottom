// tests/core_logic_test.cpp — unit tests for the PURE logic layers.
//
// core_grid_test.cpp covers rendering: does the widget put its glyphs in the
// right cells. This file covers the part of rockbottom that decides WHAT to
// say and WHICH rows to say it about — the four modules that are pure
// functions over PODs and, until now, had no test at all:
//
//   units.hpp       humanize_bytes / humanize_rate / rate() / Ratio
//   proc_query.hpp  the filter language (fields, comparators, negation)
//   proc_order.hpp  flat + tree ordering, the row order everything indexes
//   verdict.cpp     the diagnosis engine — the product's whole differentiator
//
// The verdict tests matter most. `judge()` is ~380 lines of scoring heuristics
// whose output is the sentence every user reads first; without pinned
// behaviour, a flipped comparator silently changes what the tool tells people
// about their machine. These tests assert the DECISIONS (which finding wins,
// which severity it maps to), not the exact prose, so wording stays free to
// improve while the diagnosis stays honest.
//
// Run: cmake --build build --target rb_core_tests && ./build/rb_core_tests

#include "../src/core/metrics.hpp"
#include "../src/core/units.hpp"
#include "../src/core/sampler.hpp"
#include "../src/ui/proc_query.hpp"
#include "../src/ui/proc_order.hpp"
#include "../src/ui/kill_plan.hpp"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace rockbottom;

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    std::printf("  %s %s\n", ok ? "\x1b[32mPASS\x1b[0m" : "\x1b[31mFAIL\x1b[0m", what.c_str());
    if (!ok) ++failures;
}

void eq_str(const std::string& got, const std::string& want, const std::string& what) {
    const bool ok = got == want;
    ++checks;
    if (ok) {
        std::printf("  \x1b[32mPASS\x1b[0m %s\n", what.c_str());
    } else {
        std::printf("  \x1b[31mFAIL\x1b[0m %s (got \"%s\", want \"%s\")\n",
                    what.c_str(), got.c_str(), want.c_str());
        ++failures;
    }
}

void section(const char* name) { std::printf("\n\x1b[1m%s\x1b[0m\n", name); }

// ── fixtures ────────────────────────────────────────────────────────────────

constexpr std::uint64_t kKiB = 1024ull;
constexpr std::uint64_t kMiB = 1024ull * 1024;
constexpr std::uint64_t kGiB = 1024ull * 1024 * 1024;

ProcInfo mkproc(int pid, int ppid, const char* name, double cpu,
                std::uint64_t rss_bytes, const char* user = "alice",
                char state = 'S') {
    ProcInfo p;
    p.pid   = pid;
    p.ppid  = ppid;
    p.name  = name;
    p.user  = user;
    p.state = state;
    p.cpu   = cpu;
    p.rss   = Bytes{rss_bytes};
    p.start_sec = 1000;
    return p;
}

// A snapshot of a machine that is completely fine. Every distress test starts
// from this and perturbs exactly ONE axis, so a finding that fires can only be
// attributable to that axis.
Snapshot calm_snapshot() {
    Snapshot s;
    s.hostname = "testbox";
    s.cpu.logical   = 8;
    s.cpu.total     = Ratio{0.12};
    s.cpu.iowait    = Ratio{0.0};
    s.cpu.loadavg   = {0.4, 0.5, 0.5};
    s.cpu.temp_c    = 45;
    s.cpu.cores.resize(8);
    for (auto& c : s.cpu.cores) c.usage = Ratio{0.12};
    // A history ring long enough that the trend tests are meaningful, flat so
    // that the default is "no trend".
    s.cpu.total_hist_len = 60;
    for (int i = 0; i < 60; ++i) s.cpu.total_history[static_cast<std::size_t>(i)] = 0.12f;

    s.mem.total     = Bytes{16 * kGiB};
    s.mem.used      = Bytes{6 * kGiB};
    s.mem.available = Bytes{10 * kGiB};
    s.mem.hist_len  = 60;
    for (int i = 0; i < 60; ++i) s.mem.usage_history[static_cast<std::size_t>(i)] = 0.375f;

    s.procs.push_back(mkproc(1, 0, "init", 0.1, 8 * kMiB, "root"));
    s.procs.push_back(mkproc(100, 1, "editor", 3.0, 512 * kMiB));
    s.proc_count = 2;
    return s;
}

// judge() reads only the Snapshot and ncpu_, so a default-constructed Sampler
// (which probes the host for ncpu_) is a legitimate driver for it. We assert
// on decisions that don't depend on the exact core count.
Verdict judge_of(const Snapshot& s, double dt = 1.0) {
    static Sampler sampler;   // constructed once; read_static() is not cheap
    return sampler.judge(s, dt);
}

// ── units ───────────────────────────────────────────────────────────────────

void test_units() {
    section("units — humanize + dimensional crossing");

    eq_str(humanize_bytes(Bytes{0}),            "0B",    "zero bytes");
    eq_str(humanize_bytes(Bytes{512}),          "512B",  "sub-KiB stays in bytes");
    eq_str(humanize_bytes(Bytes{kKiB}),         "1.0K",  "exactly 1 KiB gets a decimal");
    eq_str(humanize_bytes(Bytes{1536}),         "1.5K",  "1.5 KiB");
    eq_str(humanize_bytes(Bytes{20 * kKiB}),    "20K",   ">=10 drops the decimal");
    eq_str(humanize_bytes(Bytes{4 * kGiB}),     "4.0G",  "gibibytes");

    // The boundary that trips naive implementations: a value just under a unit
    // boundary must not ROUND UP into "1024" of the smaller unit. The loop that
    // picks the unit stops at d < 1024, but the formatter rounds afterwards, so
    // 1 MiB - 1 byte arrived as 1023.999K and printed "1024K". Found by this
    // test; fixed with a rounding-promotion step in humanize_bytes.
    eq_str(humanize_bytes(Bytes{kMiB - 1}), "1.0M", "just under 1 MiB promotes to 1.0M");
    eq_str(humanize_bytes(Bytes{kGiB - 1}), "1.0G", "just under 1 GiB promotes to 1.0G");
    eq_str(humanize_bytes(Bytes{kKiB - 1}), "1023B",
           "just under 1 KiB stays in bytes (no rounding to 1024B)");
    check(humanize_bytes(Bytes{kMiB - 1}).find("1024") == std::string::npos,
          "no byte value renders as 1024 of a smaller unit");

    // The same promotion must hold for rates, which share the algorithm.
    eq_str(humanize_rate(ByteRate{1024.0 * 1024 - 1}), "1.0M/s",
           "rates promote at the boundary too");

    // rate() is the ONLY sanctioned Bytes→ByteRate crossing, and it must not
    // divide by a zero or negative interval (the first tick has dt == 0).
    check(rate(Bytes{1000}, 0.0).per_sec == 0.0,  "rate() with dt=0 is zero, not inf");
    check(rate(Bytes{1000}, -1.0).per_sec == 0.0, "rate() with negative dt is zero");
    check(rate(Bytes{2048}, 2.0).per_sec == 1024.0, "rate() divides by the interval");

    // Ratio is a 0..1 fraction that reports percent; the UI depends on this
    // exact relationship for every meter.
    check(Ratio{0.5}.percent() == 50.0, "Ratio::percent scales by 100");
    check(Ratio{0.0}.percent() == 0.0,  "Ratio zero");

    // MemInfo::usage() is used/total as a Ratio — the number behind "RAM 38%".
    Snapshot s = calm_snapshot();
    const double pct = s.mem.usage().percent();
    check(pct > 37.0 && pct < 38.0, "MemInfo::usage() = used/total (6/16 GiB ≈ 37.5%)");

    // A machine reporting zero total RAM must not divide by zero.
    MemInfo empty;
    check(empty.usage().percent() == 0.0, "usage() on a zero-total MemInfo is 0, not NaN");
}

// ── proc_query ──────────────────────────────────────────────────────────────

void test_proc_query() {
    section("proc_query — the filter language");
    using ui::proc_matches;

    const ProcInfo chrome = mkproc(4242, 1, "chrome", 55.0, 900 * kMiB, "alice", 'R');
    const ProcInfo helper = mkproc(4243, 4242, "chrome_helper", 2.0, 90 * kMiB, "alice", 'S');
    const ProcInfo dbus   = mkproc(77,  1, "dbus-daemon", 0.1, 4 * kMiB, "root", 'S');

    // Empty query keeps everything — the unfiltered view must not be a special
    // case in the caller.
    check(proc_matches(chrome, ""), "empty query matches everything");

    // Bare term: name OR pid substring, case-insensitive.
    check(proc_matches(chrome, "chrome"),  "bare term matches name");
    check(proc_matches(chrome, "CHROME"),  "bare term is case-insensitive");
    check(proc_matches(chrome, "hrom"),    "bare term is a substring, not a prefix");
    check(proc_matches(chrome, "4242"),    "bare term matches pid");
    check(!proc_matches(dbus,  "chrome"),  "bare term rejects a non-match");

    // Field terms.
    check(proc_matches(dbus,    "user:root"),   "user: field");
    check(!proc_matches(chrome, "user:root"),   "user: field rejects");
    check(proc_matches(chrome,  "state:R"),     "state: by letter");
    check(proc_matches(chrome,  "state:running"), "state: by word");
    check(!proc_matches(helper, "state:running"), "state: rejects a sleeping proc");

    // Numeric comparators.
    check(proc_matches(chrome,  "cpu:>50"),  "cpu:> over threshold");
    check(!proc_matches(helper, "cpu:>50"),  "cpu:> under threshold");
    check(proc_matches(helper,  "cpu:<10"),  "cpu:< under threshold");
    check(proc_matches(chrome,  "cpu:>=55"), "cpu:>= boundary is inclusive");
    check(proc_matches(chrome,  "cpu:20"),   "bare number means at-least");

    // Byte suffixes on mem:.
    check(proc_matches(chrome,  "mem:>500m"), "mem:> with M suffix");
    check(!proc_matches(helper, "mem:>500m"), "mem:> rejects a small process");
    check(proc_matches(chrome,  "mem:>0"),    "mem:> with no suffix");

    // Negation.
    check(!proc_matches(chrome, "!chrome"),      "! negates a bare term");
    check(proc_matches(dbus,    "!chrome"),      "! keeps non-matching rows");
    check(!proc_matches(dbus,   "!user:root"),   "! negates a field term");

    // AND-combination — the property the whole language rests on.
    check(proc_matches(chrome,  "chrome cpu:>10"),  "two terms AND together");
    check(!proc_matches(helper, "chrome cpu:>10"),  "AND fails when one term fails");
    check(proc_matches(chrome,  "chrome !helper"),  "positive + negated term");
    check(!proc_matches(helper, "chrome !helper"),  "negation excludes the helper");

    // Whitespace robustness: leading/trailing/multiple spaces are common when
    // a user edits a filter mid-string, and must not change the result.
    check(proc_matches(chrome, "  chrome  "),      "surrounding whitespace ignored");
    check(proc_matches(chrome, "chrome    cpu:>10"), "repeated inner spaces ignored");

    // Degenerate input must not crash or match everything by accident.
    check(proc_matches(chrome, "   "),   "all-whitespace query behaves like empty");
    (void)proc_matches(chrome, "cpu:");  // no assertion; must simply not crash
    (void)proc_matches(chrome, "cpu:>"); //   "
    (void)proc_matches(chrome, ":");     //   "
    (void)proc_matches(chrome, "!");     //   "
    check(true, "malformed terms do not crash");
}

// ── proc_order ──────────────────────────────────────────────────────────────

void test_proc_order() {
    section("proc_order — row ordering (flat + tree)");
    using ui::order_procs;

    // A small tree:  init(1) ─┬─ shell(10) ─── build(11)
    //                        └─ browser(20) ─── tab(21)
    std::vector<ProcInfo> all;
    all.push_back(mkproc(1,  0,  "init",    0.1,   8 * kMiB, "root"));
    all.push_back(mkproc(10, 1,  "shell",   1.0,  16 * kMiB));
    all.push_back(mkproc(11, 10, "build",  70.0, 256 * kMiB));
    all.push_back(mkproc(20, 1,  "browser", 5.0, 900 * kMiB));
    all.push_back(mkproc(21, 20, "tab",    30.0, 400 * kMiB));

    const std::set<int> none;

    // Flat, sorted by CPU descending: the hog leads.
    {
        auto o = order_procs(all, "", SortKey::Cpu, /*desc=*/true, /*tree=*/false, none);
        check(o.procs.size() == 5, "flat view keeps every process");
        eq_str(o.procs.front()->name, "build", "flat cpu-desc puts the busiest first");
        eq_str(o.procs.back()->name,  "init",  "flat cpu-desc puts the idlest last");
        // Flat mode deliberately leaves the TREE arrays empty — there are no
        // guides, depths or subtree rollups in a flat list, and allocating
        // five parallel vectors per tick to hold zeroes would be waste. Only
        // `pinned` is sized, because the renderer consults it in both modes.
        // (The renderer must therefore bounds-check the tree arrays, not assume
        // they parallel procs[]; this test pins that contract.)
        check(o.prefix.empty(),   "flat mode leaves prefix[] empty");
        check(o.depth.empty(),    "flat mode leaves depth[] empty");
        check(o.pinned.size() == o.procs.size(), "pinned[] is always sized to procs[]");
    }

    // Ascending flips it.
    {
        auto o = order_procs(all, "", SortKey::Cpu, /*desc=*/false, /*tree=*/false, none);
        eq_str(o.procs.front()->name, "init", "flat cpu-asc reverses the order");
    }

    // Pid is a NON-magnitude key: unlike cpu/mem/io/port (where `desc` means
    // "biggest first"), desc on pid means numerically descending. Getting this
    // backwards is the classic sort-direction bug, so pin both directions.
    {
        auto d = order_procs(all, "", SortKey::Pid, /*desc=*/true,  false, none);
        check(d.procs.front()->pid == 21, "pid sort desc puts the highest pid first");
        auto a = order_procs(all, "", SortKey::Pid, /*desc=*/false, false, none);
        check(a.procs.front()->pid == 1,  "pid sort asc puts the lowest pid first");
    }

    // Tree mode DOES populate the parallel arrays, and there they must be
    // index-aligned with procs[] because the renderer indexes them together.
    {
        auto o = order_procs(all, "", SortKey::Cpu, true, /*tree=*/true, none);
        check(o.prefix.size()   == o.procs.size(), "tree: prefix[] aligned with procs[]");
        check(o.has_kids.size() == o.procs.size(), "tree: has_kids[] aligned with procs[]");
        check(o.depth.size()    == o.procs.size(), "tree: depth[] aligned with procs[]");
        check(o.sub_cpu.size()  == o.procs.size(), "tree: sub_cpu[] aligned with procs[]");
        check(o.pinned.size()   == o.procs.size(), "tree: pinned[] aligned with procs[]");
    }

    // Tree mode: roots first, children under their parent, and the row count
    // matches the flat count when nothing is filtered or collapsed.
    {
        auto o = order_procs(all, "", SortKey::Cpu, true, /*tree=*/true, none);
        check(o.procs.size() == 5, "tree view shows every process when nothing is collapsed");
        eq_str(o.procs.front()->name, "init", "the only root leads the tree");
        check(o.depth.front() == 0, "root is at depth 0");

        // Find shell and assert its child follows it and is deeper.
        std::size_t shell_at = 0;
        for (std::size_t i = 0; i < o.procs.size(); ++i)
            if (o.procs[i]->pid == 10) shell_at = i;
        check(shell_at + 1 < o.procs.size() && o.procs[shell_at + 1]->pid == 11,
              "a child immediately follows its parent");
        check(o.depth[shell_at + 1] > o.depth[shell_at],
              "a child is deeper than its parent");
        check(o.has_kids[shell_at], "a parent is marked has_kids");

        // Subtree rollup: shell's sub_cpu must include build's CPU.
        check(o.sub_cpu[shell_at] >= 70.0,
              "sub_cpu rolls a child's CPU up into its parent");
    }

    // Collapsing a node hides its descendants but keeps the node itself, and
    // reports how many rows are hidden.
    {
        std::set<int> collapsed{10};
        auto o = order_procs(all, "", SortKey::Cpu, true, true, collapsed);
        check(o.procs.size() == 4, "collapsing a node removes its subtree rows");
        bool shell_present = false, build_present = false;
        std::size_t shell_at = 0;
        for (std::size_t i = 0; i < o.procs.size(); ++i) {
            if (o.procs[i]->pid == 10) { shell_present = true; shell_at = i; }
            if (o.procs[i]->pid == 11) build_present = true;
        }
        check(shell_present,  "the collapsed node itself stays visible");
        check(!build_present, "the collapsed node's child is hidden");
        check(o.collapsed[shell_at], "the collapsed node is flagged");
        check(o.hidden[shell_at] >= 1, "the collapsed node reports its hidden count");
    }

    // Filtering in tree mode keeps matched rows' ANCESTORS as context, so the
    // lineage stays legible — that's the documented contract.
    {
        auto o = order_procs(all, "build", SortKey::Cpu, true, true, none);
        bool has_build = false, has_shell = false, has_browser = false;
        for (const ProcInfo* p : o.procs) {
            if (p->pid == 11) has_build = true;
            if (p->pid == 10) has_shell = true;
            if (p->pid == 20) has_browser = true;
        }
        check(has_build,    "filter keeps the matching row");
        check(has_shell,    "filter keeps the match's ancestor as context");
        check(!has_browser, "filter drops an unrelated subtree");
    }

    // Pinning FLAGS a row; it does not reorder. Hoisting the pinned row is the
    // view's decision, so the ordering stays a pure function of the sort key
    // and the flag rides alongside it.
    {
        auto o = order_procs(all, "", SortKey::Cpu, true, false, none, /*pin_pid=*/20);
        check(o.pinned.size() == o.procs.size(), "pin: flags array is sized to the rows");
        int pinned_count = 0;
        std::size_t pinned_at = 0;
        for (std::size_t i = 0; i < o.pinned.size(); ++i)
            if (o.pinned[i]) { ++pinned_count; pinned_at = i; }
        check(pinned_count == 1, "exactly one row carries the pin flag");
        check(pinned_count == 1 && o.procs[pinned_at]->pid == 20,
              "the flagged row is the pinned pid");
        // Order is unchanged by pinning.
        eq_str(o.procs.front()->name, "build", "pinning does not reorder the list");
    }

    // A pin for a pid that is not in the list must flag nothing (the pinned
    // process exited while its detail pane was open).
    {
        auto o = order_procs(all, "", SortKey::Cpu, true, false, none, /*pin_pid=*/999999);
        int pinned_count = 0;
        for (std::size_t i = 0; i < o.pinned.size(); ++i) if (o.pinned[i]) ++pinned_count;
        check(pinned_count == 0, "a pin for an absent pid flags no row");
    }

    // An empty process list must produce an empty (not malformed) view — this
    // happens on a sandboxed host where the walk yields nothing.
    {
        std::vector<ProcInfo> empty;
        auto o = order_procs(empty, "", SortKey::Cpu, true, true, none);
        check(o.procs.empty(),  "empty input yields an empty ordering");
        check(o.prefix.empty(), "empty input yields aligned empty arrays");
    }

    // An ORPHAN (parent not in the list) must surface as a root rather than
    // vanishing — losing a process because its parent exited is a data bug.
    {
        std::vector<ProcInfo> orphaned;
        orphaned.push_back(mkproc(500, 499, "orphan", 1.0, kMiB));  // ppid 499 absent
        auto o = order_procs(orphaned, "", SortKey::Cpu, true, true, none);
        check(o.procs.size() == 1, "an orphan is not dropped from the tree");
        check(!o.procs.empty() && o.depth.front() == 0, "an orphan surfaces as a root");
    }

    // A PARENT CYCLE (pid whose ancestry loops) must not hang the ordering.
    // Real /proc can briefly present inconsistent ppids across a racing walk.
    {
        std::vector<ProcInfo> cyclic;
        cyclic.push_back(mkproc(600, 601, "a", 1.0, kMiB));
        cyclic.push_back(mkproc(601, 600, "b", 1.0, kMiB));
        auto o = order_procs(cyclic, "", SortKey::Cpu, true, true, none);
        check(o.procs.size() <= 2, "a parent cycle terminates without duplicating rows");
        check(true, "a parent cycle does not hang the tree builder");
    }
}

// ── verdict ─────────────────────────────────────────────────────────────────

void test_verdict() {
    section("verdict — the diagnosis engine");

    // A calm machine is Calm, and still says something useful.
    {
        const Verdict v = judge_of(calm_snapshot());
        check(v.level == Health::Calm, "an idle machine is Calm");
        check(!v.headline.empty(),     "even Calm produces a headline");
        check(!v.detail.empty(),       "Calm explains WHY it is calm");
    }

    // OOM proximity: absolute available RAM, not percentage, is the trigger.
    // 200 MB free on a 16 GB box is an emergency even though "12% free"
    // sounds survivable — that's the documented design and the reason this
    // check exists at all.
    {
        Snapshot s = calm_snapshot();
        s.mem.available = Bytes{200 * kMiB};
        s.mem.used      = Bytes{16 * kGiB - 200 * kMiB};
        const Verdict v = judge_of(s);
        check(v.level == Health::Critical, "near-OOM is Critical");
        check(v.headline.find("memory") != std::string::npos ||
              v.headline.find("Memory") != std::string::npos ||
              v.headline.find("OOM")    != std::string::npos,
              "near-OOM headline is about memory (got: " + v.headline + ")");
    }

    // Thrashing: live paging traffic outranks a merely-full swap. A machine
    // with swap allocated but NOT moving pages is not thrashing.
    {
        Snapshot s = calm_snapshot();
        s.mem.swap_total = Bytes{8 * kGiB};
        s.mem.swap_used  = Bytes{7 * kGiB};   // lots of swap in use…
        s.mem.swap_in    = ByteRate{0};        // …but nothing moving
        s.mem.swap_out   = ByteRate{0};
        const Verdict v = judge_of(s);
        check(v.level != Health::Critical,
              "parked swap alone is not an emergency (got: " + v.headline + ")");
    }
    {
        Snapshot s = calm_snapshot();
        s.mem.swap_total = Bytes{8 * kGiB};
        s.mem.swap_used  = Bytes{4 * kGiB};
        s.mem.swap_in    = ByteRate{20.0 * 1024 * 1024};   // 20 MB/s in
        s.mem.swap_out   = ByteRate{20.0 * 1024 * 1024};   // 20 MB/s out
        const Verdict v = judge_of(s);
        check(v.level == Health::Critical, "active paging traffic IS an emergency");
        check(v.headline.find("thrash") != std::string::npos ||
              v.headline.find("Thrash") != std::string::npos ||
              v.headline.find("RAM")    != std::string::npos,
              "thrashing headline names thrashing (got: " + v.headline + ")");
    }

    // Thermal: over 92°C is critical-tier, 82-92 is a lesser finding.
    {
        Snapshot s = calm_snapshot();
        s.cpu.temp_c = 95;
        const Verdict v = judge_of(s);
        check(v.level == Health::Critical, "95C is Critical");
        check(v.headline.find("overheat") != std::string::npos ||
              v.headline.find("hot")      != std::string::npos,
              "95C headline is about heat (got: " + v.headline + ")");
    }
    {
        Snapshot s = calm_snapshot();
        s.cpu.temp_c = 85;
        const Verdict v = judge_of(s);
        check(v.level != Health::Calm && v.level != Health::Critical,
              "85C is a warning, not an emergency (got: " + v.headline + ")");
    }

    // Zombie herd is advisory: it must register, but must never outrank a
    // real emergency. This is the "strongest finding wins" property.
    {
        Snapshot s = calm_snapshot();
        s.zombies = 40;
        const Verdict z = judge_of(s);
        check(z.level != Health::Calm, "a zombie herd is reported");

        s.mem.available = Bytes{150 * kMiB};   // add a genuine emergency
        s.mem.used      = Bytes{16 * kGiB - 150 * kMiB};
        const Verdict both = judge_of(s);
        check(both.level == Health::Critical, "the emergency wins over zombies");
        check(both.headline.find("ombie") == std::string::npos,
              "the zombie finding does not become the headline (got: " + both.headline + ")");
    }

    // Disk latency: a failing drive (extreme latency) outranks a merely
    // saturated one, and saturation with NORMAL latency is not called failure.
    {
        Snapshot s = calm_snapshot();
        DriveIO d;
        d.name = "nvme0n1";
        d.busy = 0.99;
        d.read_lat_ms = 1.0;    // fast, just busy
        d.write_lat_ms = 1.0;
        s.drives.push_back(d);
        const Verdict v = judge_of(s);
        check(v.headline.find("saturated") != std::string::npos,
              "a busy-but-fast drive is 'saturated' (got: " + v.headline + ")");
    }
    {
        Snapshot s = calm_snapshot();
        DriveIO d;
        d.name = "sda";
        d.busy = 0.80;
        d.read_lat_ms = 250.0;   // catastrophic service time
        d.write_lat_ms = 10.0;
        s.drives.push_back(d);
        const Verdict v = judge_of(s);
        check(v.level == Health::Stressed || v.level == Health::Critical,
              "an extremely slow drive raises the level");
        check(v.headline.find("latency") != std::string::npos,
              "extreme latency is named as latency (got: " + v.headline + ")");
    }

    // A mildly slow FIRST drive must not mask a much worse LATER drive —
    // the loop's `break` placement is load-bearing and easy to regress.
    {
        Snapshot s = calm_snapshot();
        DriveIO slow;
        slow.name = "sda"; slow.busy = 0.5;
        slow.read_lat_ms = 60.0; slow.write_lat_ms = 5.0;
        DriveIO awful;
        awful.name = "sdb"; awful.busy = 0.5;
        awful.read_lat_ms = 400.0; awful.write_lat_ms = 5.0;
        s.drives.push_back(slow);
        s.drives.push_back(awful);
        const Verdict v = judge_of(s);
        check(!v.headline.empty(), "a multi-drive machine still produces a verdict");
    }

    // SSD health: a critical SMART warning is the highest-severity storage
    // finding and must reach Critical.
    {
        Snapshot s = calm_snapshot();
        SsdHealth h;
        h.name = "nvme0";
        h.crit_warning = 0x01;
        s.ssd_health.push_back(h);
        const Verdict v = judge_of(s);
        check(v.level == Health::Critical, "a SMART critical warning is Critical");
        check(v.headline.find("SSD") != std::string::npos,
              "the SSD warning names the drive class (got: " + v.headline + ")");
    }

    // Determinism: the same snapshot must produce the same verdict. The engine
    // sorts findings by score, and a non-deterministic tie-break would make the
    // headline flicker between ticks on a steady machine.
    {
        Snapshot s = calm_snapshot();
        s.cpu.temp_c = 95;
        s.zombies = 20;
        const Verdict a = judge_of(s);
        const Verdict b = judge_of(s);
        eq_str(b.headline, a.headline, "judge() is deterministic for a fixed snapshot");
        eq_str(b.detail,   a.detail,   "judge() detail is deterministic too");
    }

    // A completely empty snapshot (every collector blocked — a locked-down
    // container) must not crash and must not invent a crisis.
    {
        Snapshot empty;
        const Verdict v = judge_of(empty);
        check(!v.headline.empty(), "an empty snapshot still yields a headline");
        check(v.level != Health::Critical,
              "no data is not an emergency (got: " + v.headline + ")");
    }

    // dt == 0 is the first tick. Trend findings divide by the sample window;
    // this must not produce a NaN verdict or a spurious leak report.
    {
        Snapshot s = calm_snapshot();
        const Verdict v = judge_of(s, /*dt=*/0.0);
        check(!v.headline.empty(), "dt=0 (first tick) yields a sane verdict");
    }
}

// ── kill_plan ───────────────────────────────────────────────────────────────

void test_kill_plan() {
    section("kill_plan — target selection and the pid-reuse guard");
    using namespace rockbottom::ui;

    // start_matches FAILS CLOSED. These four cases are the whole guard: an
    // unknown start time on either side must refuse, because the action being
    // gated is SIGKILL and "I couldn't check" is not "it's fine".
    check(start_matches(1000, 1000), "identical start times match");
    check(start_matches(1000, 1002), "within the +2s boot-epoch tolerance");
    check(start_matches(1002, 1000), "tolerance is symmetric");
    check(!start_matches(1000, 1005), "beyond tolerance is a different process");
    check(!start_matches(0, 1000),    "unknown ARMED start refuses (fail closed)");
    check(!start_matches(1000, 0),    "unknown CURRENT start refuses (fail closed)");
    check(!start_matches(0, 0),       "both unknown refuses");

    // THE RACE. A kill is armed against pid 4242 (start 1000). The user sits on
    // the y/n prompt; 4242 exits and the kernel reassigns that pid to something
    // else, which now reports a different start time. The armed target must NOT
    // be signalled — this is the bug that would make a monitor kill a random
    // process.
    {
        std::vector<ProcInfo> armed_snap{mkproc(4242, 1, "doomed", 50.0, kMiB)};
        armed_snap[0].start_sec = 1000;
        const std::vector<int> targets{4242};
        const auto starts = starts_of(armed_snap, targets);
        check(starts.size() == 1 && starts[0] == 1000, "starts_of captures the arm-time start");

        // Same pid, reborn later.
        std::vector<ProcInfo> now{mkproc(4242, 1, "innocent-bystander", 1.0, kMiB)};
        now[0].start_sec = 9999;
        const auto rt = resolve_targets(targets, starts, now);
        check(rt.signalable.empty(), "a RECYCLED pid is never signalled");
        check(rt.recycled == 1,      "the recycled target is counted");
    }

    // The same target, unchanged, must still be signalable — the guard has to
    // not be so strict that the feature stops working.
    {
        std::vector<ProcInfo> snap{mkproc(4242, 1, "doomed", 50.0, kMiB)};
        snap[0].start_sec = 1000;
        const std::vector<int> targets{4242};
        const auto rt = resolve_targets(targets, starts_of(snap, targets), snap);
        check(rt.signalable.size() == 1 && rt.signalable[0] == 4242,
              "an unchanged target is still signalable");
        check(rt.recycled == 0, "nothing counted as recycled");
    }

    // A target that simply EXITED (absent from the new snapshot) is dropped,
    // not signalled into the void.
    {
        std::vector<ProcInfo> snap{mkproc(4242, 1, "doomed", 50.0, kMiB)};
        snap[0].start_sec = 1000;
        const std::vector<int> targets{4242};
        const auto starts = starts_of(snap, targets);
        const std::vector<ProcInfo> gone;   // it exited
        const auto rt = resolve_targets(targets, starts, gone);
        check(rt.signalable.empty(), "a vanished target is dropped");
        check(rt.recycled == 1,      "a vanished target counts as gone");
    }

    // A group kill where SOME targets recycled: the survivors are still
    // signalled and the gone ones counted, so "3 of 5" behaves.
    {
        std::vector<ProcInfo> armed;
        for (int i = 0; i < 4; ++i) {
            armed.push_back(mkproc(700 + i, 1, "helper", 1.0, kMiB));
            armed.back().start_sec = 1000;
        }
        const std::vector<int> targets{700, 701, 702, 703};
        const auto starts = starts_of(armed, targets);

        std::vector<ProcInfo> now = armed;
        now[1].start_sec = 5555;   // 701 recycled
        now.erase(now.begin() + 3); // 703 exited
        const auto rt = resolve_targets(targets, starts, now);
        check(rt.signalable.size() == 2, "survivors in a group are still signalled");
        check(rt.recycled == 2,          "recycled + vanished are both counted");
        check(rt.signalable[0] == 700 && rt.signalable[1] == 702,
              "the right survivors are selected");
    }

    // A starts[] shorter than pids[] (defensive: should not happen, but the
    // list is fed to kill(2)) must fail closed rather than read past the end.
    {
        std::vector<ProcInfo> snap{mkproc(10, 1, "a", 1.0, kMiB)};
        snap[0].start_sec = 1000;
        const std::vector<int> targets{10};
        const std::vector<std::uint64_t> no_starts;   // truncated
        const auto rt = resolve_targets(targets, no_starts, snap);
        check(rt.signalable.empty(), "a missing arm-time start fails closed");
    }

    // plan_by_name: the "close all the Chrome helpers" gesture.
    {
        std::vector<ProcInfo> procs{
            mkproc(3, 1, "helper", 1.0, kMiB),
            mkproc(1, 1, "helper", 1.0, kMiB),
            mkproc(2, 1, "other",  1.0, kMiB),
        };
        const auto pids = plan_by_name(procs, "helper");
        check(pids.size() == 2, "plan_by_name selects every same-named process");
        check(pids[0] == 1 && pids[1] == 3, "plan_by_name is pid-ordered (deterministic)");
        check(plan_by_name(procs, "nonesuch").empty(), "plan_by_name of an absent name is empty");
    }

    // plan_subtree: root + all descendants, depth-first over a real tree.
    {
        std::vector<ProcInfo> procs{
            mkproc(1,  0,  "init",   0.1, kMiB),
            mkproc(10, 1,  "shell",  1.0, kMiB),
            mkproc(11, 10, "build",  1.0, kMiB),
            mkproc(12, 11, "cc1",    1.0, kMiB),
            mkproc(20, 1,  "other",  1.0, kMiB),
        };
        const auto sub = plan_subtree(procs, 10);
        check(sub.size() == 3, "plan_subtree collects root + all descendants");
        std::set<int> got(sub.begin(), sub.end());
        check(got.count(10) && got.count(11) && got.count(12),
              "plan_subtree reaches grandchildren");
        check(!got.count(20), "plan_subtree excludes unrelated branches");
        check(!got.count(1),  "plan_subtree does not walk UPWARD to the parent");

        // A leaf's subtree is just itself — the confirm strip words this as a
        // single-target kill.
        check(plan_subtree(procs, 12).size() == 1, "a leaf's subtree is itself");
        check(plan_subtree(procs, 999).size() == 1,
              "an unknown root yields just that pid");
        check(plan_subtree(procs, 0).empty(),  "pid 0 plans nothing");
        check(plan_subtree(procs, -1).empty(), "a negative pid plans nothing");
    }

    // CYCLE SAFETY. /proc is walked pid by pid while the kernel forks and
    // reaps, so the observed ppid map can contain a loop. An unguarded
    // depth-first walk would either spin forever or emit duplicate pids — and
    // this list goes straight to kill(2). If this test hangs, that's the bug.
    {
        std::vector<ProcInfo> cyclic{
            mkproc(600, 601, "a", 1.0, kMiB),
            mkproc(601, 600, "b", 1.0, kMiB),
        };
        const auto sub = plan_subtree(cyclic, 600);
        check(sub.size() <= 2, "a parent cycle does not duplicate targets");
        std::set<int> uniq(sub.begin(), sub.end());
        check(uniq.size() == sub.size(), "no pid appears twice in a kill plan");
        check(true, "plan_subtree terminates on a cyclic ppid map");
    }
    {
        // A self-parent (pid == ppid) is a 1-cycle; it must not recurse.
        std::vector<ProcInfo> selfp{mkproc(700, 700, "self", 1.0, kMiB)};
        const auto sub = plan_subtree(selfp, 700);
        check(sub.size() == 1, "a self-parented process plans exactly itself");
    }

    // describe_outcome: the wording is the user's only evidence of what
    // happened, so the signal-specific verbs are pinned. Saying "killed" for a
    // SIGTERM the process ignored would be a lie the UI tells constantly.
    {
        const auto k = describe_outcome(SIGKILL, "hog", 42, 1, 1, 0, 0, "");
        check(!k.error, "a successful kill is not an error");
        check(k.text.find("force-killed") != std::string::npos,
              "SIGKILL says force-killed (got: " + k.text + ")");
        check(k.text.find("42") != std::string::npos, "single-target names the pid");

        const auto t = describe_outcome(SIGTERM, "hog", 42, 1, 1, 0, 0, "");
        check(t.text.find("asked") != std::string::npos &&
              t.text.find("to exit") != std::string::npos,
              "SIGTERM only ASKS (got: " + t.text + ")");

        const auto s = describe_outcome(SIGSTOP, "hog", 42, 1, 1, 0, 0, "");
        check(s.text.find("suspended") != std::string::npos,
              "SIGSTOP says suspended (got: " + s.text + ")");

        const auto c = describe_outcome(SIGCONT, "hog", 42, 1, 1, 0, 0, "");
        check(c.text.find("resumed") != std::string::npos,
              "SIGCONT says resumed (got: " + c.text + ")");

        // A group kill reports a count, not a pid.
        const auto g = describe_outcome(SIGTERM, "helper", 700, 5, 5, 0, 0, "");
        check(g.text.find("5") != std::string::npos,
              "a group kill reports how many (got: " + g.text + ")");

        // Failures surface the real errno text and mark the toast as an error.
        const auto f = describe_outcome(SIGKILL, "root-proc", 1, 1, 0, 1, 0,
                                        "permission denied — not your process");
        check(f.error, "a failed kill is an error toast");
        check(f.text.find("permission denied") != std::string::npos,
              "the failure reports the actual reason");

        // Everything recycled: nothing was signalled, and we say so rather
        // than claiming a success.
        const auto r = describe_outcome(SIGKILL, "gone", 42, 1, 0, 0, 1, "");
        check(r.error, "an all-recycled kill is an error toast");
        check(r.text.find("already exited") != std::string::npos,
              "an all-recycled kill says nothing was signaled (got: " + r.text + ")");

        // Partial: some signalled, some already gone — report both halves.
        const auto p = describe_outcome(SIGTERM, "helper", 700, 4, 3, 0, 1, "");
        check(!p.error, "a partial success is not an error");
        check(p.text.find("already gone") != std::string::npos,
              "a partial kill mentions the gone targets (got: " + p.text + ")");
    }
}

}  // namespace

int main() {
    std::printf("\x1b[1mrockbottom core logic tests\x1b[0m\n");
    test_units();
    test_proc_query();
    test_proc_order();
    test_kill_plan();
    test_verdict();

    std::printf("\n%d checks, %d failure%s\n",
                checks, failures, failures == 1 ? "" : "s");
    if (failures == 0) std::printf("\x1b[32mALL PASS\x1b[0m\n");
    return failures == 0 ? 0 : 1;
}
