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
    bool two_up_ = true;   // pack mounts two per row (wide terminals)

public:
    DiskPanel(const std::vector<DiskInfo>& d, const DiskIO& io, bool two_up = true)
        : disks_(d), io_(io), two_up_(two_up) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;

        // ── Row 1: live whole-system I/O rates with peak-normalized history ──
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
                text("I/O") | nowrap | Bold | fgc(pal::disk_ac) | w_<5>,
                text("read ▼") | nowrap | fgc(pal::sky),
                text(humanize_rate(io_.read)) | nowrap | Bold | fgc(pal::text) | w_<8>,
                Spark{rn.data(), io_.hist_len}.cells(16).color(pal::sky),
                text("   write ▲") | nowrap | fgc(pal::pink),
                text(humanize_rate(io_.write)) | nowrap | Bold | fgc(pal::text) | w_<8>,
                Spark{wn.data(), io_.hist_len}.cells(16).color(pal::pink)
            ) | gap(1)).build());
        }

        // ── Mounts: two per row across the full width ──
        if (disks_.empty()) {
            rows.push_back((text("no mounted disks") | fgc(pal::dim)).build());
        } else {
            auto mount_cell = [&](const DiskInfo& d) -> Element {
                const double f = d.usage().v;
                std::string mnt = d.mount.size() > 12
                    ? "…" + d.mount.substr(d.mount.size() - 11) : d.mount;
                return (h(
                    text(mnt) | nowrap | fgc(pal::disk_ac) | w_<13>,
                    Meter{f}.width(14),
                    text(fmt::pct(f)) | nowrap | fgc(load_color(f)) | w_<5>,
                    text(humanize_bytes(d.used) + " / " + humanize_bytes(d.total))
                        | nowrap | fgc(pal::text) | w_<12>,
                    text(d.fstype) | nowrap | fgc(pal::dim) | w_<6>
                ) | gap(1)).build();
            };
            const int n = static_cast<int>(disks_.size());
            const int step = two_up_ ? 2 : 1;
            for (int i = 0; i < n; i += step) {
                std::vector<Element> line;
                line.push_back(mount_cell(disks_[static_cast<std::size_t>(i)]));
                if (two_up_ && i + 1 < n)
                    line.push_back(mount_cell(disks_[static_cast<std::size_t>(i + 1)]));
                rows.push_back((h(line) | gap(3)).build());
            }
        }

        return Panel("◇", "DISK", pal::disk_ac)(std::move(rows));
    }
};

}  // namespace bottom::ui
