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
        // right-side vitals as width shrinks so it never wraps into the banner
        // below. Widest-first budget check keyed off the ComponentElement's w.
        std::string host   = snap_.hostname;
        std::string kernel = snap_.kernel;
        std::string uptime = std::string(humanize_duration(snap_.uptime_sec));
        std::string procs  = std::to_string(snap_.proc_count);
        std::string runs_s = std::to_string(snap_.running);
        const bool has_bat = snap_.battery.present;
        const bool paused = paused_;

        return Element{ComponentElement{
            .render = [=](int w, int) -> Element {
                // Fixed left cluster: pulse(6) + " rockbottom"(11) + " rb"(3).
                // Each optional segment is added only if it still fits.
                int need = 6 + 11 + 3;
                const bool show_host   = (need += 2 + static_cast<int>(host.size()),   w >= need);
                const bool show_kernel = show_host &&
                                         (need += 2 + static_cast<int>(kernel.size()), w >= need);
                const bool show_bat    = has_bat &&
                                         (need += 6, w >= need);
                const bool show_uptime = (need += 4 + static_cast<int>(uptime.size()), w >= need);
                const bool show_procs  = show_uptime &&
                                         (need += 5 + static_cast<int>(procs.size())
                                                    + static_cast<int>(runs_s.size()) + 8, w >= need);

                std::vector<Element> cols;
                cols.push_back(pulse);
                cols.push_back(word);
                cols.push_back(tag);
                if (show_host)
                    cols.push_back((text("  " + host) | nowrap | Bold | fgc(pal::sky)).build());
                if (show_kernel)
                    cols.push_back((text("  " + kernel) | nowrap | fgc(pal::dim)).build());
                if (paused) { cols.push_back((text("  ")).build()); cols.push_back(pause_chip); }
                cols.push_back(space);
                if (show_bat) cols.push_back(bat);
                if (show_uptime)
                    cols.push_back((text("  up " + uptime) | nowrap | fgc(pal::label)).build());
                if (show_procs) {
                    cols.push_back((text("  ·  ") | nowrap | fgc(pal::faint)).build());
                    cols.push_back((text(procs) | nowrap | fgc(pal::text)).build());
                    cols.push_back((text(" procs ") | nowrap | fgc(pal::dim)).build());
                    cols.push_back((text(runs_s) | nowrap | fgc(pal::good)).build());
                    cols.push_back((text(" running") | nowrap | fgc(pal::dim)).build());
                }
                return (h(std::move(cols))).build();
            },
        }};
    }
};

}  // namespace rockbottom::ui
