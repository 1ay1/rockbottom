// main.cpp — entry point: hand the App program to the maya runtime.

#include <maya/maya.hpp>

#include "ui/app.hpp"

int main() {
    maya::run<rockbottom::App>({
        .title = "rockbottom",
        .mouse = true,
        .mode  = maya::Mode::Fullscreen,
    });
    return 0;
}
