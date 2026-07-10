// widgets/footer.hpp — key hints strip, live indicator, toast notifications,
// and context-sensitive hints (filter mode / kill pending).

#pragma once

#include <maya/maya.hpp>

#include "../state.hpp"
#include "../theme.hpp"

#include <string>
#include <vector>

namespace rockbottom::ui {

class Footer {
    bool paused_;
    int ticks_;
    const Toast* toast_;
    const PendingKill* pending_;
    bool filtering_;
    std::string filter_;

public:
    Footer(bool paused, int ticks, const Toast* toast,
           const PendingKill* pending, bool filtering, std::string filter)
        : paused_(paused), ticks_(ticks), toast_(toast),
          pending_(pending), filtering_(filtering), filter_(std::move(filter)) {}

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
        // StatusBar idiom: a thin rail separator between logical hint groups
        // so the strip reads as segments, not one long word soup.
        auto sep = []() -> Element {
            return (text(" │") | nowrap | fgc(pal::faint)).build();
        };

        std::vector<Element> parts;

        if (pending_) {
            parts.push_back(hint("y", "confirm"));
            parts.push_back(hint("n", "cancel"));
        } else if (filtering_) {
            parts.push_back((text(" filtering: ") | nowrap | fgc(pal::dim)).build());
            parts.push_back((text("/" + filter_ + "▌") | nowrap | Bold | fgc(pal::sky)).build());
            parts.push_back(hint("enter", "apply"));
            parts.push_back(hint("esc", "clear"));
        } else {
            // Groups: app │ navigate │ act on process │ view
            parts.push_back(hint("q", "quit"));
            parts.push_back(sep());
            parts.push_back(hint("↑↓", "select"));
            parts.push_back(hint("/", "filter"));
            parts.push_back(sep());
            parts.push_back(hint("x", "end"));
            parts.push_back(hint("K", "kill"));
            parts.push_back(hint("s", "sort"));
            parts.push_back(sep());
            parts.push_back(hint("1-6", "detail"));
            parts.push_back(hint("space", "pause"));
            parts.push_back(hint("?", "help"));
        }

        // Toast overrides the live indicator on the right.
        Element status;
        if (toast_) {
            Color c = toast_->error ? pal::crit : pal::good;
            status = (text(" " + toast_->text + " ")
                      | nowrap | Bold | fgc(pal::bg) | bgc(c)).build();
        } else if (paused_) {
            status = (text(" ⏸ paused ") | nowrap | Bold | fgc(pal::bg) | bgc(pal::warn)).build();
        } else {
            // Heartbeat: a braille spinner that advances one frame per tick
            // (sysmon-example idiom) — proof of life, not just a static dot.
            static constexpr const char* kSpin[] =
                {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
            status = (h(text(std::string(kSpin[ticks_ % 10]) + " ") | nowrap | fgc(pal::good),
                        text("live " + std::to_string(ticks_)) | nowrap | fgc(pal::dim))).build();
        }

        return (h(h(parts) | gap(1), space, std::move(status), text(" "))
                | bgc(pal::bg_panel) | padding(0, 1, 0, 1)).build();
    }
};

}  // namespace rockbottom::ui
