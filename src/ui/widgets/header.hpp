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
#include <cstdio>
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
            std::string icon = b.charging ? "\u26a1" : "\u25a0";
            std::string s = "  " + icon + " " + std::to_string(b.percent) + "%";
            if (b.temp_c > 0.0f) {
                char t[16]; std::snprintf(t, sizeof t, " %.0f\xc2\xb0", b.temp_c);
                s += t;
            }
            bat = (text(s) | nowrap | fgc(bc)).build();
        }

        // Wireless chip: WiFi SSID + signal, else cellular type. Only present
        // on Android/Termux where the platform reports it; empty otherwise.
        Element wifi = blank();
        {
            const auto& w = snap_.wireless;
            if (w.wifi_present) {
                // RSSI (dBm) → coarse bars. -50 great, -80 poor.
                Color wc = w.wifi_rssi >= -60 ? pal::good
                         : w.wifi_rssi >= -75 ? pal::warn : pal::crit;
                std::string s = "  \xf0\x9f\x93\xb6 " + w.ssid;
                if (w.wifi_rssi != 0) s += " " + std::to_string(w.wifi_rssi) + "dBm";
                if (w.link_mbps > 0)  s += " " + std::to_string(w.link_mbps) + "M";
                wifi = (text(s) | nowrap | fgc(wc)).build();
            } else if (w.cell_present && !w.net_type.empty()) {
                std::string s = "  \xf0\x9f\x93\xb6 " + w.net_type;
                if (!w.operator_name.empty()) s += " " + w.operator_name;
                wifi = (text(s) | nowrap | fgc(pal::label)).build();
            }
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
        {
            const auto& w = snap_.wireless;
            if (w.wifi_present || (w.cell_present && !w.net_type.empty()))
                items.push_back({std::move(wifi), 3});
        }
        if (snap_.battery.present) items.push_back({std::move(bat), 3});
        items.push_back({(text("  up " + std::string(humanize_duration(snap_.uptime_sec)))
                          | nowrap | fgc(pal::label)).build(), 2});
        items.push_back({std::move(procs_cluster), 1});
        return fit_row(std::move(items));
    }
};

}  // namespace rockbottom::ui
