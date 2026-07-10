// widgets/header.hpp — top strip: gradient wordmark · host · kernel · vitals.

#pragma once

#include <maya/maya.hpp>
#include <maya/widget/gradient.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"

#include <string>

namespace bottom::ui {

class Header {
    const Snapshot& snap_;
    bool paused_;

public:
    Header(const Snapshot& s, bool paused) : snap_(s), paused_(paused) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        Element word = gradient("▁▂▃ bottom", pal::mauve, pal::blue, /*bold=*/true);

        auto pause_chip = paused_
            ? (text(" ⏸ PAUSED ") | Bold | fgc(pal::bg) | bgc(pal::warn)).build()
            : blank();

        // Battery chip, only when hardware exists.
        Element bat = blank();
        if (snap_.battery.present) {
            const auto& b = snap_.battery;
            Color bc = b.charging ? pal::good : b.percent < 20 ? pal::crit
                     : b.percent < 40 ? pal::warn : pal::label;
            std::string icon = b.charging ? "⚡" : "■";
            bat = (text("  " + icon + " " + std::to_string(b.percent) + "%")
                   | nowrap | fgc(bc)).build();
        }

        return (h(
            std::move(word),
            text("  " + snap_.hostname) | Bold | fgc(pal::sky),
            text("  " + snap_.kernel) | fgc(pal::dim),
            text("  ") , std::move(pause_chip),
            space,
            std::move(bat),
            text("  up " + humanize_duration(snap_.uptime_sec)) | fgc(pal::label),
            text("  ·  ") | fgc(pal::faint),
            text(std::to_string(snap_.proc_count)) | fgc(pal::text),
            text(" procs ") | fgc(pal::dim),
            text(std::to_string(snap_.running)) | fgc(pal::good),
            text(" running") | fgc(pal::dim)
        )).build();
    }
};

}  // namespace bottom::ui
