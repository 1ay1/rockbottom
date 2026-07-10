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

namespace rockbottom::ui {

class NetPanel {
    const std::vector<NetIface>& nets_;

public:
    explicit NetPanel(const std::vector<NetIface>& n) : nets_(n) {}

    // The pane always shows the top 4 interfaces (the sampler sorts by
    // traffic rate, name as tiebreak) — a stable roster that never grows a
    // dozen rows of dead utun tunnels. Everything past 4 folds into one
    // dim summary line.
    static constexpr int kMaxRows = 4;

    // Rows the card body occupies. Layout math in app.hpp must use this,
    // not nets.size().
    [[nodiscard]] static int rows(const std::vector<NetIface>& nets) {
        if (nets.empty()) return 1;
        const int n = static_cast<int>(nets.size());
        return std::min(n, kMaxRows) + (n > kMaxRows ? 1 : 0);
    }

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;
        if (nets_.empty())
            rows.push_back((text("no active interfaces") | fgc(pal::dim)).build());

        std::vector<const NetIface*> live;
        std::vector<std::string> idle_names;
        for (const auto& n : nets_) {
            if (static_cast<int>(live.size()) < kMaxRows) live.push_back(&n);
            else idle_names.push_back(n.name);
        }

        for (const auto* np : live) {
            const auto& n = *np;
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

            std::string name = std::string(fmt::clip(n.name, 7));
            std::string rx = std::string(humanize_rate(n.rx));
            std::string tx = std::string(humanize_rate(n.tx));
            std::array<float, 48> rxa = rxn, txa = txn;
            int hl = n.hist_len;
            rows.push_back(Element{ComponentElement{
                .render = [=](int w, int) -> Element {
                    // Fixed columns: name(8) + ▼(1)+rx(7)+gap + ▲(2)+tx(7) +
                    // gaps. Whatever's left feeds the two sparks equally.
                    const bool show_spark = w >= 34;
                    int fixed = 8 + 1 + 7 + 2 + 7 + 5;   // labels + gaps
                    int slack = std::max(0, w - fixed);
                    int each = show_spark ? slack / 2 : 0;
                    std::vector<Element> cols;
                    cols.push_back((text(name) | nowrap | Bold | fgc(pal::net_ac) | w_<8>).build());
                    cols.push_back((text("▼") | nowrap | fgc(pal::good)).build());
                    cols.push_back((text(rx) | nowrap | fgc(pal::text) | w_<7>).build());
                    if (each > 0)
                        cols.push_back(Spark{rxa.data(), hl}.cells(each).color(pal::good)
                                           .baseline(true).build_fixed());
                    cols.push_back((text("▲") | nowrap | fgc(pal::hot)).build());
                    cols.push_back((text(tx) | nowrap | fgc(pal::text) | w_<7>).build());
                    if (each > 0)
                        cols.push_back(Spark{txa.data(), hl}.cells(each).color(pal::hot)
                                           .baseline(true).build_fixed());
                    return (h(std::move(cols)) | gap(1)).build();
                },
            }});
        }

        if (!idle_names.empty()) {
            std::size_t count = idle_names.size();
            std::vector<std::string> names = idle_names;
            rows.push_back(Element{ComponentElement{
                .render = [count, names](int w, int) -> Element {
                    std::string label = std::to_string(count) + " more";
                    std::string list;
                    std::size_t shown = 0;
                    int budget = std::max(0, w - static_cast<int>(label.size()) - 4);
                    for (const auto& nm : names) {
                        int need = static_cast<int>(nm.size()) + (list.empty() ? 0 : 1);
                        if (static_cast<int>(list.size()) + need > budget) break;
                        if (!list.empty()) list += ' ';
                        list += nm;
                        ++shown;
                    }
                    if (shown < names.size()) {
                        std::string more = " +" + std::to_string(names.size() - shown);
                        while (!list.empty() &&
                               static_cast<int>(list.size() + more.size()) > budget) {
                            std::size_t sp = list.find_last_of(' ');
                            if (sp == std::string::npos) { list.clear(); break; }
                            list.resize(sp);
                        }
                        list += more;
                    }
                    return (h(
                        text(label) | nowrap | fgc(pal::dim) | w_<8>,
                        text(list) | nowrap | fgc(mix(pal::dim, pal::bg_panel, 0.35))
                    ) | gap(1)).build();
                },
            }});
        }

        return Panel("⇅", "NETWORK", pal::net_ac)(std::move(rows));
    }
};

}  // namespace rockbottom::ui
