// widgets/cpu_panel.hpp — CPU card: total meter + per-core meters and
// value-colored history sparklines, temperature chip on the border.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"
#include "meter.hpp"
#include "spark.hpp"
#include "panel.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace bottom::ui {

class CpuPanel {
    const CpuInfo& cpu_;
    int cols_;   // 2 = roomy (meter+spark), 3 = compact (meter only)

public:
    explicit CpuPanel(const CpuInfo& c, int cols = 2) : cpu_(c), cols_(std::max(2, cols)) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;

        // ── Summary row: ALL meter + wide total history ──
        // (load averages live on the verdict banner's chip — not repeated here)
        const double tf = cpu_.total.v;
        rows.push_back((h(
            text("ALL") | Bold | fgc(pal::cpu_ac) | w_<4>,
            Meter{tf}.width(16),
            text(fmt::pct(tf)) | nowrap | Bold | fgc(load_color(tf)) | w_<5>,
            Spark{cpu_.total_history.data(), cpu_.total_hist_len}.cells(28)
        ) | gap(1)).build());

        // ── Per-core grid: cols_ columns, one line each ──
        const int n = static_cast<int>(cpu_.cores.size());
        const int per_col = (n + cols_ - 1) / cols_;
        const bool compact = cols_ > 2;
        auto cell = [&](int i) -> Element {
            const CpuCore& c = cpu_.cores[static_cast<std::size_t>(i)];
            const double f = c.usage.v;
            char id[8];
            std::snprintf(id, sizeof id, "%2d", i);
            if (compact)
                return (h(
                    text(id) | nowrap | fgc(pal::cpu_ac) | w_<3>,
                    Meter{f}.width(8),
                    text(fmt::pct(f)) | nowrap | fgc(load_color(f)) | w_<4>
                ) | gap(1)).build();
            return (h(
                text(id) | nowrap | fgc(pal::cpu_ac) | w_<3>,
                Meter{f}.width(8),
                text(fmt::pct(f)) | nowrap | fgc(load_color(f)) | w_<4>,
                Spark{c.history.data(), c.hist_len}.cells(8)
            ) | gap(1)).build();
        };
        for (int r = 0; r < per_col; ++r) {
            std::vector<Element> line;
            for (int col = 0; col < cols_; ++col) {
                int i = col * per_col + r;
                if (i < n) line.push_back(cell(i));
            }
            rows.push_back((h(line) | gap(3)).build());
        }

        // Temp chip on the border.
        std::string chip;
        if (cpu_.temp_c > 1) chip = std::to_string(static_cast<int>(cpu_.temp_c)) + "°C";

        return Panel("◈", "CPU · " + fmt::short_model(cpu_.model), pal::cpu_ac)
            .chip(chip)(std::move(rows));
    }
};

}  // namespace bottom::ui
