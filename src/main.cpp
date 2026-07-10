// main.cpp — entry point: hand the App program to the maya runtime.

#include <maya/maya.hpp>

#include "ui/app.hpp"

int main() {
    maya::run<bottom::App>({
        .title = "bottom",
        .mouse = false,
        .mode  = maya::Mode::Fullscreen,
    });
    return 0;
}
