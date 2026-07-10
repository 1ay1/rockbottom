// widgets/net_panel.hpp — network card: per-interface ▼rx/▲tx live rates with
// peak-normalized rx+tx sparklines and lifetime totals.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"
#include "spark.hpp"
#include "panel.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace bottom::ui {

class NetPanel {
    const std::vector<NetIface>& nets_;

public:
    explicit NetPanel(const std::vector<NetIface>& n) : nets_(n) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;
        if (nets_.empty())
            rows.push_back((text("no active interfaces") | fgc(pal::dim)).build());

        for (const auto& n : nets_) {
            // Normalize each iface against its own recent peak so the shape
            // is visible whether it's B/s or MB/s traffic.
            float peak = 1.0f;
            for (int i = 0; i < n.hist_len; ++i)
                peak = std::max({peak,
                                 n.rx_history[static_cast<std::size_t>(i)],
                                 n.tx_history[static_cast<std::size_t>(i)]});
            std::array<float, 48> rxn{}, txn{};
            for (int i = 0; i < n.hist_len; ++i) {
                rxn[static_cast<std::size_t>(i)] = n.rx_history[static_cast<std::size_t>(i)] / peak;
                txn[static_cast<std::size_t>(i)] = n.tx_history[static_cast<std::size_t>(i)] / peak;
            }

            rows.push_back((h(
                text(fmt::clip(n.name, 7)) | nowrap | Bold | fgc(pal::net_ac) | w_<8>,
                text("▼") | nowrap | fgc(pal::good),
                text(humanize_rate(n.rx)) | nowrap | fgc(pal::text) | w_<7>,
                Spark{rxn.data(), n.hist_len}.cells(8).color(pal::good),
                text(" ▲") | nowrap | fgc(pal::hot),
                text(humanize_rate(n.tx)) | nowrap | fgc(pal::text) | w_<7>,
                Spark{txn.data(), n.hist_len}.cells(8).color(pal::hot)
            ) | gap(1)).build());
        }

        return Panel("⇅", "NETWORK", pal::net_ac)(std::move(rows));
    }
};

}  // namespace bottom::ui
