// widgets/net_panel.hpp — network card: per-interface ▼rx/▲tx live rates with
// peak-normalized rx+tx sparklines and lifetime totals. When the card is wide
// enough it splits into two responsive columns — interfaces on the left, the
// busiest active connections (who you're talking to) on the right — so the
// "who is this machine talking to" answer lives on the dashboard, not only in
// the drill-down. Below the split width it degrades to interfaces only, with a
// one-line connection tally, so the row budget never grows.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"
#include "spark.hpp"
#include "panel.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace rockbottom::ui {

class NetPanel {
    const std::vector<NetIface>&    nets_;
    const std::vector<Connection>*  conns_ = nullptr;

public:
    explicit NetPanel(const std::vector<NetIface>& n) : nets_(n) {}
    NetPanel(const std::vector<NetIface>& n, const std::vector<Connection>& c)
        : nets_(n), conns_(&c) {}

    // The pane always shows the top 4 interfaces (the sampler sorts by
    // traffic rate, name as tiebreak) — a stable roster that never grows a
    // dozen rows of dead utun tunnels. Everything past 4 folds into one
    // dim summary line.
    static constexpr int kMaxRows = 4;

    // Card body width at/above which the connections column appears beside the
    // interfaces instead of collapsing to a single tally line. Below this the
    // interface rows already want every column, so a second column would just
    // truncate both — we keep it single-column and honest.
    static constexpr int kSplitWidth = 50;

    // Rows the card body occupies. Layout math in app.hpp must use this, not
    // nets.size(). The connections column rides ALONGSIDE the interface rows
    // (a horizontal split), so it never adds height — the tally line only shows
    // when narrow, and it reuses the roster's own row budget.
    [[nodiscard]] static int rows(const std::vector<NetIface>& nets) {
        return nets.empty() ? 1
             : std::min(static_cast<int>(nets.size()), kMaxRows)
               + (static_cast<int>(nets.size()) > kMaxRows ? 1 : 0);
    }

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        // ── left column: the interface roster ──
        std::vector<Element> iface_rows = interface_rows();
        const int iface_count = static_cast<int>(iface_rows.size());
        auto ifaces = std::make_shared<std::vector<Element>>(std::move(iface_rows));

        // ── right column data: busiest active connections ──
        // ESTABLISHED first (real traffic), then LISTEN (servers). Remote-first
        // ordering keeps the "who am I talking to" answer at the top.
        std::vector<Connection> conn_copy;
        int established = 0, listening = 0;
        if (conns_) {
            for (const auto& c : *conns_) {
                if (c.state == "ESTABLISHED") ++established;
                else if (c.state == "LISTEN") ++listening;
            }
            for (const auto& c : *conns_)
                if (c.state == "ESTABLISHED") conn_copy.push_back(c);
            for (const auto& c : *conns_)
                if (c.state == "LISTEN") conn_copy.push_back(c);
        }
        const int total_conns = established + listening;

        // Build the body as a width-driven ComponentElement so the split is
        // truly responsive to the card's actual rendered width.
        Element body = Element{ComponentElement{
            .render = [ifaces, conn_copy, total_conns, established, listening,
                       iface_count](int w, int) -> Element {
                using namespace maya;
                using namespace maya::dsl;

                std::vector<Element> left = *ifaces;

                // Narrow: interfaces only. A single dim tally line summarizes
                // the socket table you'd get in the detail pane.
                if (w < kSplitWidth || conn_copy.empty()) {
                    if (total_conns > 0) {
                        std::string tally = std::to_string(established) + " active";
                        if (listening > 0) tally += " · " + std::to_string(listening) + " listening";
                        left.push_back((h(
                            text("sockets") | nowrap | fgc(pal::dim) | w_<8>,
                            text(tally) | nowrap | fgc(mix(pal::dim, pal::bg_panel, 0.35))
                        ) | gap(1)).build());
                    }
                    return v(std::move(left)).build();
                }

                // Wide: two columns side by side. The interface roster keeps
                // ~62% of the width; connections take the rest.
                const int gap_w  = 2;
                const int right_w = std::clamp((w - gap_w) * 38 / 100, 22, 40);
                const int left_w  = w - gap_w - right_w;

                // Right column: a header + one connection per interface row so
                // the two columns stay the same height (no dangling rows).
                std::vector<Element> right;
                std::string hdr = std::to_string(established) + " active";
                if (listening > 0) hdr += " · " + std::to_string(listening) + " listen";
                right.push_back((text(hdr) | nowrap | fgc(pal::net_ac)).build());

                const int slots = std::max(1, iface_count - 1);
                const int shown = std::min<int>(static_cast<int>(conn_copy.size()), slots);
                for (int i = 0; i < shown; ++i) {
                    const Connection& c = conn_copy[static_cast<std::size_t>(i)];
                    const bool est = c.state == "ESTABLISHED";
                    // Remote endpoint is the interesting half for ESTABLISHED;
                    // for LISTEN show the bound local port.
                    std::string endp = est ? c.raddr
                                            : (c.laddr.empty() ? "*" : c.laddr);
                    Color ep_c = est ? pal::sky : pal::good;
                    std::string who = c.pname.empty()
                        ? (c.pid > 0 ? std::to_string(c.pid) : std::string("—"))
                        : std::string(fmt::clip(c.pname, 10));
                    right.push_back((h(
                        text(est ? "→" : "◇") | nowrap | fgc(ep_c),
                        text(std::string(fmt::clip(endp, 20))) | nowrap
                            | fgc(est ? pal::text : pal::label) | grow(1),
                        text(who) | nowrap | fgc(pal::dim)
                    ) | gap(1)).build());
                }
                const int extra = static_cast<int>(conn_copy.size()) - shown;
                if (extra > 0)
                    right.push_back((text("+" + std::to_string(extra) + " more")
                                         | nowrap | fgc(pal::dim)).build());

                return (h(
                    v(std::move(left))  | width(left_w),
                    v(std::move(right)) | width(right_w)
                ) | gap(gap_w)).build();
            },
        }};

        std::vector<Element> body_rows;
        body_rows.push_back(std::move(body));
        return Panel("⇅", "NETWORK", pal::net_ac)(std::move(body_rows));
    }

private:
    // Build the per-interface rows (the left column / whole body when narrow).
    [[nodiscard]] std::vector<maya::Element> interface_rows() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;
        if (nets_.empty()) {
            rows.push_back((text("no active interfaces") | fgc(pal::dim)).build());
            return rows;
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

        return rows;
    }
};

}  // namespace rockbottom::ui
