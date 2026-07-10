// widgets/footer.hpp — key hints strip with live/paused indicator.

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"

#include <string>
#include <vector>

namespace bottom::ui {

class Footer {
    bool paused_;
    int ticks_;

public:
    Footer(bool paused, int ticks) : paused_(paused), ticks_(ticks) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        auto hint = [](const char* k, const char* d) -> Element {
            return (h(
                text(std::string(" ") + k) | nowrap | Bold | fgc(pal::sky),
                text(std::string("·") + d) | nowrap | fgc(pal::dim)
            )).build();
        };

        std::vector<Element> parts = {
            hint("q", "quit"), hint("space", "pause"), hint("s", "sort"),
            hint("c", "cpu"), hint("m", "mem"), hint("n", "name"), hint("?", "help"),
        };

        auto live = paused_
            ? (text(" ⏸ paused ") | nowrap | Bold | fgc(pal::bg) | bgc(pal::warn)).build()
            : (h(text("◉ ") | nowrap | fgc(pal::good),
                 text("live " + std::to_string(ticks_)) | nowrap | fgc(pal::dim))).build();

        return (h(h(parts) | gap(1), space, std::move(live), text(" "))
                | bgc(pal::bg_panel) | padding(0, 1, 0, 1)).build();
    }
};

}  // namespace bottom::ui
