// widgets/disk_panel.hpp — filesystem card: deduped real mounts, gradient
// usage meters, capacity figures.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"
#include "meter.hpp"
#include "spark.hpp"
#include "panel.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace bottom::ui {

class DiskPanel {
    const std::vector<DiskInfo>& disks_;
    const DiskIO& io_;

public:
    DiskPanel(const std::vector<DiskInfo>& d, const DiskIO& io) : disks_(d), io_(io) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;

        // ── Live whole-system I/O rates with peak-normalized history ──
        {
            float peak = 1.0f;
            for (int i = 0; i < io_.hist_len; ++i)
                peak = std::max({peak,
                                 io_.read_history[static_cast<std::size_t>(i)],
                                 io_.write_history[static_cast<std::size_t>(i)]});
            std::array<float, 48> rn{}, wn{};
            for (int i = 0; i < io_.hist_len; ++i) {
                rn[static_cast<std::size_t>(i)] = io_.read_history[static_cast<std::size_t>(i)] / peak;
                wn[static_cast<std::size_t>(i)] = io_.write_history[static_cast<std::size_t>(i)] / peak;
            }
            rows.push_back((h(
                text("I/O") | nowrap | Bold | fgc(pal::disk_ac) | w_<14>,
                text("▼") | nowrap | fgc(pal::sky),
                text(humanize_rate(io_.read)) | nowrap | fgc(pal::text) | w_<7>,
                Spark{rn.data(), io_.hist_len}.cells(8).color(pal::sky),
                text(" ▲") | nowrap | fgc(pal::pink),
                text(humanize_rate(io_.write)) | nowrap | fgc(pal::text) | w_<7>,
                Spark{wn.data(), io_.hist_len}.cells(8).color(pal::pink)
            ) | gap(1)).build());
        }

        if (disks_.empty())
            rows.push_back((text("no mounted disks") | fgc(pal::dim)).build());

        for (const auto& d : disks_) {
            const double f = d.usage().v;
            std::string mnt = d.mount.size() > 13
                ? "…" + d.mount.substr(d.mount.size() - 12) : d.mount;
            rows.push_back((h(
                text(mnt) | nowrap | fgc(pal::disk_ac) | w_<14>,
                Meter{f}.width(12),
                text(fmt::pct(f)) | nowrap | fgc(load_color(f)) | w_<5>,
                text(humanize_bytes(d.used) + " / " + humanize_bytes(d.total))
                    | nowrap | fgc(pal::text) | w_<13>,
                text(d.fstype) | nowrap | fgc(pal::dim)
            ) | gap(1)).build());
        }

        return Panel("◇", "DISK", pal::disk_ac)(std::move(rows));
    }
};

}  // namespace bottom::ui
