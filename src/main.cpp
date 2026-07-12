// main.cpp — entry point: parse CLI flags + config, then hand the App program
// to the maya runtime.

#include <maya/maya.hpp>

#include "ui/app.hpp"
#include "core/config.hpp"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    using namespace rockbottom;

    // --no-config bypasses the persisted file (fresh defaults + flags only).
    bool no_config = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--no-config") == 0) no_config = true;

    // Precedence: defaults < config file < CLI flags.
    Config cfg = no_config ? Config{} : Config::load();
    std::string exit_msg;
    if (!Config::parse_args(argc, argv, cfg, exit_msg)) {
        std::fputs(exit_msg.c_str(), stdout);
        return exit_msg.rfind("unknown", 0) == 0 ? 2 : 0;
    }
    App::boot_config() = cfg;

    maya::run<rockbottom::App>({
        .title = "rockbottom",
        .fps   = 30,     // smooth animation ceiling; visual_hash still skips
                         // unchanged frames, so a static screen renders ~0 fps
                         // and idle CPU stays near zero.
        .mouse = true,
        .hover_motion = true,   // report bare hover so detail rows highlight
        .mode  = maya::Mode::Fullscreen,
    });
    return 0;
}
