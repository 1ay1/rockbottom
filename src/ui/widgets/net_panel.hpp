// widgets/net_panel.hpp — network card: per-interface ▼rx/▲tx live rates with
// peak-normalized rx+tx sparklines and lifetime totals.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"
#include "spark.hpp"
#include "graph.hpp"
#include "panel.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace rockbottom::ui {

class NetPanel {
    const std::vector<NetIface>& nets_;
    int graph_h_ = 0;   // >0: draw a throughput area-graph this tall on top
    float grow_ = 0;    // >0: panel fills its flex slot; graph fills the slack

public:
    explicit NetPanel(const std::vector<NetIface>& n, int graph_h = 0)
        : nets_(n), graph_h_(std::max(0, graph_h)) {}

    // Fill mode: the panel grows to its flex slot and the throughput mountain
    // fills whatever height is left after the iface rows. (Named `expand`, not
    // `grow`, so it doesn't shadow dsl::grow inside build().)
    NetPanel& expand(float g) { grow_ = g; return *this; }

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

        // Pick the busiest interface for the mountain (shared by fill mode
        // and the fixed-height graph mode below).
        const NetIface* busy = nets_.empty() ? nullptr : &nets_.front();
        if (busy) {
            double best = -1;
            for (const auto& n : nets_) {
                double r = n.rx.per_sec + n.tx.per_sec;
                if (r > best) { best = r; busy = &n; }
            }
        }

        // Fill mode: a throughput mountain that expands to consume the height
        // left after the iface rows. fill() hands the render the REAL (w, h)
        // so the graph is exactly as tall as its slot — no estimate to drift.
        if (grow_ > 0 && busy) {
            float peak = 1.0f;
            for (int i = 0; i < busy->hist_len; ++i)
                peak = std::max({peak,
                                 busy->rx_history[static_cast<std::size_t>(i)],
                                 busy->tx_history[static_cast<std::size_t>(i)]});
            const float* rxh = busy->rx_history.data();
            const float* txh = busy->tx_history.data();
            const int hl = busy->hist_len;
            rows.push_back(fill([rxh, txh, hl, peak](int w, int ah) -> Element {
                using namespace maya;
                using namespace maya::dsl;
                if (ah < 2) return blank().build();
                // Normalize into a per-thread scratch; build_fixed() bakes the
                // glyphs immediately so the buffer needn't outlive this call.
                static thread_local std::array<float, 48> rxn{}, txn{};
                for (int i = 0; i < hl && i < 48; ++i) {
                    rxn[static_cast<std::size_t>(i)] = rxh[static_cast<std::size_t>(i)] / peak;
                    txn[static_cast<std::size_t>(i)] = txh[static_cast<std::size_t>(i)] / peak;
                }
                std::string peak_lbl = std::string(humanize_rate(ByteRate{peak}));
                std::vector<Element> axis;
                for (int r = 0; r < ah; ++r) {
                    std::string lbl = r == 0 ? peak_lbl : r == ah - 1 ? "0" : "";
                    axis.push_back((text(lbl) | nowrap | fgc(pal::faint)
                                    | w_<6> | justify(Justify::End)).build());
                }
                const int cells = std::max(1, w - 6 - 1);   // axis(6) + gap(1)
                Graph g{rxn.data(), hl};
                g.cells(cells).rows(ah).color(pal::good).line_only().gamma(0.5f)
                 .overlay(txn.data(), hl, pal::hot);
                return (h(v(std::move(axis)) | w_<6>, Element{g.build_fixed()})
                        | gap(1)).build();
            }, 0, 2));
        }
        // Wide/graph mode: a throughput mountain (busiest iface's rx as the
        // fill, tx as an overlay line) above the per-iface rate rows. The
        // y-axis is labelled with the window PEAK so the mountain's height is
        // an actual rate, not an unreadable normalized squiggle.
        else if (graph_h_ >= 2 && busy) {
            float peak = 1.0f;
            for (int i = 0; i < busy->hist_len; ++i)
                peak = std::max({peak,
                                 busy->rx_history[static_cast<std::size_t>(i)],
                                 busy->tx_history[static_cast<std::size_t>(i)]});
            static thread_local std::array<float, 48> rxn{}, txn{};
            for (int i = 0; i < busy->hist_len; ++i) {
                rxn[static_cast<std::size_t>(i)] = busy->rx_history[static_cast<std::size_t>(i)] / peak;
                txn[static_cast<std::size_t>(i)] = busy->tx_history[static_cast<std::size_t>(i)] / peak;
            }
            // y-axis: peak rate at top (right-aligned to 5 cells), 0 at floor.
            std::string peak_lbl = std::string(humanize_rate(ByteRate{peak}));
            std::vector<Element> axis;
            for (int r = 0; r < graph_h_; ++r) {
                std::string lbl = r == 0 ? peak_lbl : r == graph_h_ - 1 ? "0" : "";
                axis.push_back((text(lbl) | nowrap | fgc(pal::faint)
                                | w_<6> | justify(Justify::End)).build());
            }
            Graph g{rxn.data(), busy->hist_len};
            g.fill().rows(graph_h_).color(pal::good).line_only().gamma(0.5f)
             .overlay(txn.data(), busy->hist_len, pal::hot);
            rows.push_back((h(
                v(std::move(axis)) | w_<6>,
                Element{g} | grow(1)
            ) | gap(1) | height(graph_h_)).build());
        }

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

            // Idle interfaces recede: name drops to dim, zero figures to
            // faint — so the one iface actually moving bytes owns the card.
            const bool live_now = n.rx.per_sec + n.tx.per_sec >= 1.0;
            const Color name_c = live_now ? pal::net_ac : pal::dim;
            const Color rx_c = n.rx.per_sec >= 1.0 ? pal::text : pal::faint;
            const Color tx_c = n.tx.per_sec >= 1.0 ? pal::text : pal::faint;
            const Color rxg_c = n.rx.per_sec >= 1.0 ? pal::good : mix(pal::good, pal::bg_panel, 0.5);
            const Color txg_c = n.tx.per_sec >= 1.0 ? pal::hot : mix(pal::hot, pal::bg_panel, 0.5);

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
                    cols.push_back((text(name) | nowrap | Bold | fgc(name_c) | w_<8>).build());
                    cols.push_back((text("▼") | nowrap | fgc(rxg_c)).build());
                    cols.push_back((text(rx) | nowrap | fgc(rx_c) | w_<7>).build());
                    if (each > 0)
                        cols.push_back(Spark{rxa.data(), hl}.cells(each).color(pal::good)
                                           .baseline(true).build_fixed());
                    cols.push_back((text("▲") | nowrap | fgc(txg_c)).build());
                    cols.push_back((text(tx) | nowrap | fgc(tx_c) | w_<7>).build());
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

        return Panel("⇅", "NETWORK", pal::net_ac).grow(grow_)(std::move(rows));
    }
};

}  // namespace rockbottom::ui
