// main.cpp — entry point: parse CLI flags + config, then hand the App program
// to the maya runtime.

#include <maya/maya.hpp>

#include "ui/app.hpp"
#include "core/config.hpp"
#include "core/sampler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    using namespace rockbottom;

    // --no-config bypasses the persisted file (fresh defaults + flags only).
    bool no_config = false;
    bool topology  = false;
    bool bench     = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-config") == 0) no_config = true;
        if (std::strcmp(argv[i], "--topology") == 0) topology = true;
        if (std::strcmp(argv[i], "--bench") == 0) bench = true;
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
        // Match the app's real call (app.hpp kTopN == 0 = keep every process,
        // because the UI scrolls/tree-views the full list). Passing a small
        // top_n here would under-measure the sort + ProcInfo build the running
        // program actually pays each tick.
        constexpr int kBenchTopN = 0;
        (void)sampler.sample(cfg.sort, kBenchTopN, /*fast=*/true);   // prime: cold caches

        constexpr int kIters = 40;
        std::vector<double> ms;
        ms.reserve(kIters);
        for (int i = 0; i < kIters; ++i) {
            const auto t0 = clock::now();
            const Snapshot s = sampler.sample(cfg.sort, kBenchTopN, /*fast=*/false);
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
        (void)sampler.sample(cfg.sort, 1, /*fast=*/true);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const Snapshot s = sampler.sample(cfg.sort, 1, /*fast=*/true);
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
        .mode  = maya::Mode::Fullscreen,
    });
    return 0;
}
