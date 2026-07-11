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

        // Responsive: the header packs left-to-right and DROPS the optional
        // vitals as width shrinks so it never wraps into the banner below.
        // maya's fit_row measures each REAL styled fragment and sheds the
        // lowest `keep` rank first — no hand-summed cell estimates to drift.
        // Drop order (first → last): procs · uptime · battery · kernel · host.
        Element procs_cluster = (h(
            text("  ·  ") | nowrap | fgc(pal::faint),
            text(std::to_string(snap_.proc_count)) | nowrap | fgc(pal::text),
            text(" procs ") | nowrap | fgc(pal::dim),
            text(std::to_string(snap_.running)) | nowrap | fgc(pal::good),
            text(" running") | nowrap | fgc(pal::dim)
        )).build();

        std::vector<FitItem> items;
        items.push_back({std::move(pulse)});                    // essential
        items.push_back({std::move(word)});
        items.push_back({std::move(tag)});
        items.push_back({(text("  " + snap_.hostname) | nowrap | Bold
                          | fgc(pal::sky)).build(), 5});
        items.push_back({(text("  " + snap_.kernel) | nowrap
                          | fgc(pal::dim)).build(), 4});
        if (paused_)
            items.push_back({(h(text("  "), std::move(pause_chip))).build()});
        items.push_back({Element{space}});                      // grow spacer
        if (snap_.battery.present) items.push_back({std::move(bat), 3});
        items.push_back({(text("  up " + std::string(humanize_duration(snap_.uptime_sec)))
                          | nowrap | fgc(pal::label)).build(), 2});
        items.push_back({std::move(procs_cluster), 1});
        return fit_row(std::move(items));
    }
};

}  // namespace rockbottom::ui
