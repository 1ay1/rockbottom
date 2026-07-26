// main.cpp — entry point: parse CLI flags + config, then hand the App program
// to the maya runtime.

#include <maya/maya.hpp>

#include "ui/app.hpp"
#include "core/config.hpp"
#include "core/sampler.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    using namespace rockbottom;

    // --no-config bypasses the persisted file (fresh defaults + flags only).
    bool no_config = false;
    bool topology  = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-config") == 0) no_config = true;
        if (std::strcmp(argv[i], "--topology") == 0) topology = true;
    }

    // Precedence: defaults < config file < CLI flags.
    Config cfg = no_config ? Config{} : Config::load();
    std::string exit_msg;
    bool exit_ok = false;   // true only for --help / --version
    if (!Config::parse_args(argc, argv, cfg, exit_msg, exit_ok)) {
        std::fputs(exit_msg.c_str(), exit_ok ? stdout : stderr);
        return exit_ok ? 0 : 2;
    }
    App::boot_config() = cfg;

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
        .fps   = 30,     // smooth animation ceiling; visual_hash still skips
                         // unchanged frames, so a static screen renders ~0 fps
                         // and idle CPU stays near zero.
        .mouse = true,
        .mode  = maya::Mode::Fullscreen,
    });
    return 0;
}
