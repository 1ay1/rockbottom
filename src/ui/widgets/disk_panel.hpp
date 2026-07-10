// widgets/disk_panel.hpp — filesystem card: deduped real mounts, gradient
// usage meters, capacity figures.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"
#include "meter.hpp"
#include "panel.hpp"

#include <string>
#include <vector>

namespace bottom::ui {

class DiskPanel {
    const std::vector<DiskInfo>& disks_;

public:
    explicit DiskPanel(const std::vector<DiskInfo>& d) : disks_(d) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;
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
