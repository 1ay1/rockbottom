// widgets/header.hpp — top strip: live mini-spark · gradient wordmark · host ·
// kernel · vitals.

#pragma once

#include <maya/maya.hpp>
#include <maya/widget/gradient.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"
#include "spark.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace rockbottom::ui {

class Header {
    const Snapshot& snap_;
    bool paused_;

public:
    Header(const Snapshot& s, bool paused) : snap_(s), paused_(paused) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        // The brand's ▁▂▃ glyphs are alive: the last few seconds of total
        // CPU, hue-shifted through the wordmark's mauve→blue gradient.
        Element pulse = [&]() -> Element {
            const auto& hist = snap_.cpu.total_history;
            const int len = snap_.cpu.total_hist_len;
            constexpr int kCells = 6;
            std::string content;
            std::vector<StyledRun> runs;
            static constexpr const char* kBars[] =
                {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
            const int start = len > kCells ? len - kCells : 0;
            const int shown = std::max(0, len - start);
            for (int i = 0; i < kCells; ++i) {
                std::size_t off = content.size();
                float v = 0.05f;
                if (i >= kCells - shown)
                    v = std::clamp(hist[static_cast<std::size_t>(start + (i - (kCells - shown)))],
                                   0.05f, 1.0f);
                content += kBars[std::clamp(static_cast<int>(v * 7.0f + 0.5f), 0, 7)];
                const double t = kCells > 1 ? static_cast<double>(i) / (kCells - 1) : 0.0;
                runs.push_back({off, content.size() - off,
                                Style{}.with_fg(mix(pal::mauve, pal::blue, t)).with_bold()});
            }
            return Element{TextElement{.content = std::move(content), .style = {},
                                       .wrap = TextWrap::NoWrap, .runs = std::move(runs)}};
        }();

        Element word = gradient(" rockbottom", pal::mauve, pal::blue, /*bold=*/true);

        Element tag = (text(" rb") | Bold | fgc(pal::faint)).build();

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
            std::move(pulse),
            std::move(word),
            std::move(tag),
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

}  // namespace rockbottom::ui
