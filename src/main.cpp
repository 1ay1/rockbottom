// main.cpp — entry point: parse CLI flags + config, then hand the App program
// to the maya runtime.

#include <maya/maya.hpp>

#include "ui/app.hpp"
#include "core/config.hpp"
#include "core/sampler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>   // getpid, _exit (selfcheck)

int main(int argc, char** argv) {
    using namespace rockbottom;

    // --no-config bypasses the persisted file (fresh defaults + flags only).
    bool no_config = false;
    bool topology  = false;
    bool bench     = false;
    bool selfcheck = false;
    bool doctor    = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-config") == 0) no_config = true;
        if (std::strcmp(argv[i], "--topology") == 0) topology = true;
        if (std::strcmp(argv[i], "--bench") == 0) bench = true;
        if (std::strcmp(argv[i], "--selfcheck") == 0) selfcheck = true;
        if (std::strcmp(argv[i], "--doctor") == 0) doctor = true;
    }

    // Precedence: defaults < config file < CLI flags.
    Config cfg = no_config ? Config{} : Config::load();
    std::string exit_msg;
    bool exit_ok = false;   // true only for --help / --version
    if (!Config::parse_args(argc, argv, cfg, exit_msg, exit_ok)) {
        std::fputs(exit_msg.c_str(), exit_ok ? stdout : stderr);
        return exit_ok ? 0 : 2;
    }

    // Validate the theme name HERE rather than in config.hpp: the palette deck
    // lives in ui/theme.hpp (a UI-layer header config.hpp deliberately doesn't
    // pull in). An unknown --theme=NAME is a user typo worth reporting on the
    // spot — the same courtesy --sort= already gets — instead of silently
    // falling back to native and, worse, PERSISTING the bad name on exit. A
    // stale name coming only from the config file stays a soft fallback (init()
    // handles that); we reject just an explicit, wrong CLI value.
    {
        bool theme_from_cli = false;
        for (int i = 1; i < argc; ++i)
            if (std::strncmp(argv[i], "--theme=", 8) == 0) theme_from_cli = true;
        if (theme_from_cli && ui::theme_index_by_name(cfg.theme) < 0) {
            std::string names;
            for (std::size_t i = 0; i < ui::theme_count(); ++i)
                names += (i ? ", " : "") + std::string(ui::theme_name(i));
            std::fprintf(stderr, "unknown theme: %s\navailable: %s\n",
                         cfg.theme.c_str(), names.c_str());
            return 2;
        }
    }
    App::boot_config() = cfg;

    // --bench: time the SAMPLER alone, with no terminal and no rendering.
    //
    // A monitor's steady-state cost is dominated by what it asks the kernel
    // for every tick, and that is the part a screenshot can't show. Timing it
    // in isolation makes optimisation measurable instead of anecdotal: run it
    // before and after a change and the difference is the change. Reports the
    // median as well as the mean because collector cost is spiky (a throttled
    // collector firing on one tick shouldn't read as a regression).
    if (bench) {
        using clock = std::chrono::steady_clock;
        Sampler sampler;
        // The sampler always keeps every process (the UI scrolls and tree-views
        // the full list), so the bench measures exactly the sort + ProcInfo
        // build the running program pays each tick — no cap to under-measure it.
        (void)sampler.sample(cfg.sort, /*fast=*/true);   // prime: cold caches

        constexpr int kIters = 40;
        std::vector<double> ms;
        ms.reserve(kIters);
        for (int i = 0; i < kIters; ++i) {
            const auto t0 = clock::now();
            const Snapshot s = sampler.sample(cfg.sort, /*fast=*/false);
            ms.push_back(std::chrono::duration<double, std::milli>(clock::now() - t0).count());
            // Sleep between samples so throttled collectors reach their due
            // time at a realistic cadence rather than being starved or spun.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            (void)s;
        }

        std::vector<double> sorted = ms;
        std::sort(sorted.begin(), sorted.end());
        double sum = 0;
        for (double d : ms) sum += d;
        std::printf("sample() x%d   mean %.2f ms   median %.2f ms   p90 %.2f ms   max %.2f ms\n",
                    kIters, sum / kIters, sorted[kIters / 2],
                    sorted[(kIters * 9) / 10], sorted.back());
        std::printf("steady-state CPU at 1s refresh ≈ %.2f%% of one core\n",
                    (sorted[kIters / 2] / 1000.0) * 100.0);
        return 0;
    }

    // --selfcheck: run the REAL sampler headlessly and assert the collected
    // data is sane. --version/--help exit before any sampling and the chroot
    // smoke test only proves the binary launches, so the process/cpu/mem
    // collectors — the hottest, most platform-specific, most recently churned
    // code — had ZERO runtime coverage. This gives CI a portable way to catch
    // a collector that crashes, hangs, or (worse) silently returns garbage on
    // a platform we can't run locally. Two samples so delta-based fields
    // (cpu%, rates) are actually computed, not left at their first-tick zero.
    if (selfcheck) {
        // Run the two priming/measuring samples on a worker with a hard
        // deadline. A collector that WEDGES (a hung syscall on a broken
        // /proc, a stuck IOReport subscription, an NSS lookup that never
        // returns) must fail this check loudly rather than hang CI until the
        // job's global timeout — "never returned" is itself a bug worth
        // catching. std::async + a timed wait gives us that watchdog without
        // pulling in a signal handler.
        auto work = std::async(std::launch::async, [&cfg]() -> Snapshot {
            Sampler sampler;
            (void)sampler.sample(cfg.sort, /*fast=*/false);   // prime deltas
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            return sampler.sample(cfg.sort, /*fast=*/false);
        });
        if (work.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
            std::fprintf(stderr,
                "selfcheck FAILED: sampler did not complete within 30s "
                "(a collector is wedged — hung syscall or deadlock)\n");
            // Don't join a stuck thread on the way out: _exit past any
            // destructor that might also block on the wedged resource.
            std::fflush(stderr);
            _exit(2);
        }
        const Snapshot s = work.get();

        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
            if (!ok) ++failures;
        };

        // The process walk is the code most recently rewritten (slurp_into /
        // reusable buffers / history gating). If the fbuf reuse or the raw
        // read loop were broken, this is where it shows: an empty list, a
        // process with no name, or a self-pid that can't find itself.
        check(!s.procs.empty(), "process list is non-empty");
        const int me = static_cast<int>(::getpid());
        bool found_self = false, all_named = true, pids_valid = true;
        double cpu_sum = 0;
        for (const ProcInfo& p : s.procs) {
            if (p.pid == me) found_self = true;
            if (p.pid <= 0) pids_valid = false;
            if (p.name.empty()) all_named = false;
            cpu_sum += p.cpu;
        }
        check(found_self, "our own pid appears in the process list");
        check(pids_valid, "every process has a positive pid");
        check(all_named, "every process has a non-empty name");
        check(cpu_sum >= 0.0 && cpu_sum < 100.0 * 4096, "aggregate process cpu% is in a sane range");

        // CPU: at least one core, total busy a valid fraction.
        check(!s.cpu.cores.empty(), "cpu reports at least one core");
        check(s.cpu.total.v >= 0.0 && s.cpu.total.v <= 1.0, "cpu total busy is a 0..1 fraction");

        // Memory: total must be real; used cannot exceed it.
        check(s.mem.total.value > 0, "memory total is non-zero");
        check(s.mem.used.value <= s.mem.total.value, "memory used does not exceed total");

        // Disks: used cannot exceed total. This is the invariant the used =
        // total - free clamp protects — an unguarded subtraction underflows to
        // ~18 EiB on a filesystem that transiently reports free > total
        // (reserved blocks, online resize, some overlay/network fs). Checking
        // it here runs the guard against every real mount on the CI runner.
        {
            bool disks_sane = true;
            for (const auto& d : s.disks)
                if (d.used.value > d.total.value) disks_sane = false;
            check(disks_sane, "no disk reports used space exceeding its total");
        }

        if (failures == 0) {
            std::printf("selfcheck OK: sampler produced sane data across %zu processes, %zu cores\n",
                        s.procs.size(), s.cpu.cores.size());
            return 0;
        }
        std::fprintf(stderr, "selfcheck FAILED: %d invariant(s) violated\n", failures);
        return 1;
    }

    // --doctor: report what every collector actually produced, and when a
    // domain is EMPTY, say WHY.
    //
    // This exists because rockbottom's collectors all degrade silently by
    // design — `if (statvfs(...) != 0) continue;` is the right behaviour for a
    // monitor, but it leaves a user staring at a blank GPU panel with no way
    // to distinguish "no GPU" from "no permission" from "the label didn't
    // match" short of running strace. --topology already proved the need for
    // exactly this on the CPU probe; --doctor generalises it to every domain.
    //
    // Output is plain text, one line per collector, so it pastes cleanly into
    // a bug report. This is the FIRST thing to ask for when someone says "the
    // X panel is empty on my machine".
    if (doctor) {
        auto work = std::async(std::launch::async, [&cfg]() -> Snapshot {
            Sampler sampler;
            (void)sampler.sample(cfg.sort, /*fast=*/false);   // prime deltas
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            return sampler.sample(cfg.sort, /*fast=*/false);
        });
        // Same watchdog discipline as --selfcheck: a wedged collector must
        // report as wedged rather than hang the diagnostic tool.
        if (work.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
            std::fprintf(stderr,
                "doctor: sampler did not complete within 30s — a collector is "
                "wedged (hung syscall or deadlock). This is itself the finding.\n");
            std::fflush(stderr);
            _exit(2);
        }
        const Snapshot s = work.get();

        std::printf("%s\n", kVersionLine);
        std::printf("host %s · kernel %s\n\n", s.hostname.c_str(), s.kernel.c_str());
        std::printf("%-12s %-4s %s\n", "COLLECTOR", "OK", "DETAIL");

        int empty_count = 0;
        // ok=true prints a count/summary; ok=false prints the REASON the domain
        // is empty, which is the entire point of this subcommand.
        //
        // The status marker is ASCII ("yes"/"no") rather than a glyph: printf's
        // %-4s pads by BYTES, so a multi-byte em-dash silently under-pads and
        // ragged the column. Plain text also survives a pipe into a bug report
        // on a terminal with no UTF-8.
        auto row = [&](const char* name, bool ok, const std::string& detail) {
            if (!ok) ++empty_count;
            std::printf("  %-10s %-4s %s\n", name, ok ? "yes" : "no", detail.c_str());
        };

        row("procs", !s.procs.empty(),
            s.procs.empty()
                ? "no processes — /proc unreadable or sandboxed (Termux without proc access)"
                : std::to_string(s.procs.size()) + " processes, " +
                  std::to_string(s.thread_count) + " threads, " +
                  std::to_string(s.running) + " running, " +
                  std::to_string(s.zombies) + " zombie");

        row("cpu", !s.cpu.cores.empty(),
            s.cpu.cores.empty()
                ? "no per-core data — /proc/stat blocked or sysctl unavailable"
                : std::to_string(s.cpu.cores.size()) + " cores, " +
                  (s.cpu.perf_cores || s.cpu.eff_cores
                       ? std::to_string(s.cpu.perf_cores) + "P+" +
                         std::to_string(s.cpu.eff_cores) + "E, "
                       : "homogeneous, ") +
                  "busy " + std::to_string(static_cast<int>(s.cpu.total.percent())) + "%");

        row("memory", s.mem.total.value > 0,
            s.mem.total.value == 0
                ? "no memory data — /proc/meminfo unreadable"
                : humanize_bytes(s.mem.total) + " total, " +
                  humanize_bytes(s.mem.available) + " available" +
                  (s.mem.swap_total.value ? ", swap " + humanize_bytes(s.mem.swap_total)
                                          : ", no swap"));

        row("disks", !s.disks.empty(),
            s.disks.empty()
                ? "no mounts — all filesystems filtered as virtual, or network "
                  "mounts skipped (they are, deliberately: statvfs can block)"
                : std::to_string(s.disks.size()) + " mount(s)");

        // Per-device I/O is a separate capability from the aggregate counters:
        // macOS has the aggregate but no /proc/diskstats equivalent, so report
        // on the DEVICE table (what the disk pane's latency view needs) and
        // mention the aggregate separately rather than conflating the two.
        row("diskio", !s.drives.empty(),
            s.drives.empty()
                ? "no per-device I/O or latency — /proc/diskstats absent (normal "
                  "on macOS; the aggregate read/write rate still works)"
                : std::to_string(s.drives.size()) + " device(s) with latency data");

        row("net", !s.nets.empty(),
            s.nets.empty()
                ? "no interfaces — /proc/net/dev or getifaddrs unavailable"
                : std::to_string(s.nets.size()) + " interface(s), " +
                  std::to_string(s.connections.size()) + " connection(s)");

        row("gpu", !s.gpus.empty(),
            s.gpus.empty()
                ? "no GPU — nvidia-smi not on PATH, no DRM card in /sys, or no "
                  "permission to read it"
                : std::to_string(s.gpus.size()) + " adapter(s): " + s.gpus.front().name);

        row("sensors", !s.sensors.empty(),
            s.sensors.empty()
                ? "no sensors — no hwmon in /sys (VM/container), or macOS "
                  "(CPU/NVMe temps need private SMC APIs)"
                : std::to_string(s.sensors.size()) + " sensor(s)" +
                  (s.cpu.temp_c > 0
                       ? ", cpu " + std::to_string(static_cast<int>(s.cpu.temp_c)) + "C"
                       : ", no cpu-zone match"));

        row("psi", s.psi.cpu.available || s.psi.mem.available || s.psi.io.available,
            (s.psi.cpu.available || s.psi.mem.available || s.psi.io.available)
                ? "kernel pressure-stall accounting available"
                : "no PSI — /proc/pressure absent (Linux <4.20, CONFIG_PSI=n, "
                  "or not Linux). Verdict falls back to load/iowait heuristics.");

        row("ssd", !s.ssd_health.empty(),
            s.ssd_health.empty()
                ? "no SMART data — NVMe admin ioctl needs root, or no NVMe device"
                : std::to_string(s.ssd_health.size()) + " drive(s) reporting endurance");

        row("battery", s.battery.present,
            s.battery.present
                ? std::to_string(s.battery.percent) + "%" +
                      (s.battery.charging ? " charging" : " on battery")
                : "no battery — desktop/server, or power supply class not exposed");

        row("wireless", s.wireless.wifi_present || s.wireless.cell_present,
            (s.wireless.wifi_present || s.wireless.cell_present)
                ? "wireless telemetry present"
                : "no wireless — expected on desktop (this is an Android/Termux "
                  "source; the net pane already shows link rates)");

        std::printf("\nverdict: %s\n", s.verdict.headline.c_str());
        std::printf("         %s\n", s.verdict.detail.c_str());

        std::printf("\n%d of 12 collectors reported no data.\n", empty_count);
        std::printf("A \"no\" row is not necessarily a bug — read its reason. "
                    "Include this\noutput when reporting an empty panel.\n");
        // Always exit 0: "this machine has no GPU" is information, not failure.
        // --selfcheck is the pass/fail gate; --doctor is the explainer.
        return 0;
    }

    // --topology: dump what the probe actually decided, as plain text, and
    // exit. This exists because the per-core work (issues #2/#3) is only as
    // good as the topology probe underneath it, and that probe reads evidence
    // that VARIES BY MACHINE — sysfs cpu_core/cpu_atom cpulists, cpu_capacity,
    // frequency tiers, hwmon labels. A reporter on hardware we don't have can
    // run this one line and paste the answer, instead of describing a TUI.
    // Pair it with RB_SYSFS_ROOT=/path/to/captured/sys to replay someone
    // else's machine locally.
    if (topology) {
        Sampler sampler;
        // Two samples: the first has no delta to divide by, so per-core load
        // would read as zero and a real "all cores busy" machine would be
        // indistinguishable from an idle one in the dump.
        (void)sampler.sample(cfg.sort, /*fast=*/true);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const Snapshot s = sampler.sample(cfg.sort, /*fast=*/true);
        const CpuInfo& c = s.cpu;

        std::printf("%s\n", kVersionLine);
        std::printf("model      %s\n", c.model.c_str());
        std::printf("logical    %d\n", c.logical);
        if (c.hetero())
            std::printf("clusters   heterogeneous — %d %s + %d %s\n",
                        c.perf_cores, c.perf_label.empty() ? "performance" : c.perf_label.c_str(),
                        c.eff_cores,  c.eff_label.empty()  ? "efficiency"  : c.eff_label.c_str());
        else
            std::printf("clusters   homogeneous (no P/E split detected)\n");
        if (c.temp_c > 0) std::printf("pkg temp   %.1f C\n", c.temp_c);
        else               std::printf("pkg temp   -\n");

        std::printf("\n%4s  %-5s  %5s  %9s  %7s  %s\n",
                    "cpu", "class", "phys", "freq", "temp", "load");
        for (std::size_t i = 0; i < c.cores.size(); ++i) {
            const CpuCore& k = c.cores[i];
            const char* cls = k.kind == CoreKind::Perf ? "P"
                            : k.kind == CoreKind::Eff  ? "E" : "-";
            char freq[16], temp[16], phys[16];
            const double ghz = static_cast<double>(k.freq.value) / 1e9;
            if (ghz > 0) std::snprintf(freq, sizeof freq, "%.2f GHz", ghz);
            else         std::snprintf(freq, sizeof freq, "%s", "-");
            if (k.temp_c > 0) std::snprintf(temp, sizeof temp, "%.1f C", k.temp_c);
            else              std::snprintf(temp, sizeof temp, "%s", "-");
            if (k.phys >= 0) std::snprintf(phys, sizeof phys, "%d", k.phys);
            else             std::snprintf(phys, sizeof phys, "%s", "-");
            std::printf("%4zu  %-5s  %5s  %9s  %7s  %3.0f%%\n",
                        i, cls, phys, freq, temp, k.usage.v * 100.0);
        }

        // The raw sensor labels too: when a per-core temp column comes back
        // empty, the question is always whether the kernel exposed nothing or
        // whether the label didn't match, and only this distinguishes them.
        std::printf("\ncpu-zone sensors (%zu)\n",
                    [&] { std::size_t n = 0; for (const Sensor& sn : s.sensors) if (sn.zone == "cpu") ++n; return n; }());
        for (const Sensor& sn : s.sensors)
            if (sn.zone == "cpu") std::printf("  %-24s %.1f C\n", sn.label.c_str(), sn.temp_c);
        return 0;
    }

    maya::run<rockbottom::App>({
        .title = "rockbottom",
        // Event-driven, NOT a fixed frame rate.
        //
        // fps=N makes maya's poll wake every 1000/N ms purely to re-check
        // whether anything changed — at 30 that is 30 wakeups a second on a
        // screen that repaints at most once per sample. Nothing here is
        // frame-animated: the footer spinner advances one step per TICK (see
        // visual_hash), and the tick, key, mouse and resize subscriptions all
        // wake the loop on their own. So 0 costs no responsiveness and lets an
        // idle monitor sit in poll() instead of spinning through a hash 30
        // times a second.
        .fps   = 0,
        .mouse = true,
        // Report bare (no-button) motion so the process table can highlight
        // the row under the pointer. maya gates this behind mode 1003; the
        // move flood is cheap here because hover_row folds into visual_hash,
        // so a move WITHIN a row is a no-op and only a row change repaints.
        .hover_motion = true,
        .mode  = maya::Mode::Fullscreen,
    });
    return 0;
}
