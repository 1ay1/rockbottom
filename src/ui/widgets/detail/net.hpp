// widgets/detail/net.hpp — the NETWORK drill-down body.
//
// More than btop's single-interface graph: every interface at once, each with
// live rx/tx sparklines, the peak rate pulled from its own history, lifetime
// byte totals, and link state — plus an aggregate throughput line so you see
// the whole machine's traffic at a glance.

#pragma once

#include "common.hpp"

namespace rockbottom::ui::detail {

// Peak of a rolling history buffer — raw B/s samples.
inline double hist_peak(const float* h, int len) {
    double mx = 0;
    for (int i = 0; i < len; ++i) mx = std::max(mx, static_cast<double>(h[i]));
    return mx;
}

inline std::vector<Element> net_body(const Snapshot& s, const Ctx& cx) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> b;

    if (s.nets.empty()) {
        b.push_back(verdict("no active interfaces — the machine is offline (or all links are down)", pal::dim));
        return b;
    }

    // Aggregate across interfaces first — the headline number.
    double agg_rx = 0, agg_tx = 0;
    std::uint64_t agg_rxt = 0, agg_txt = 0;
    int up = 0;
    for (const NetIface& ni : s.nets) {
        agg_rx += ni.rx.per_sec; agg_tx += ni.tx.per_sec;
        agg_rxt += ni.rx_total.value; agg_txt += ni.tx_total.value;
        if (ni.up) ++up;
    }
    b.push_back(section("ALL INTERFACES", pal::net_ac));
    b.push_back(kv3(
        "download", humanize_rate(ByteRate{agg_rx}), pal::sky,
        "upload", humanize_rate(ByteRate{agg_tx}), pal::good,
        "links up", std::to_string(up) + "/" + std::to_string(s.nets.size()),
        up > 0 ? pal::good : pal::dim));
    b.push_back(kv3(
        "lifetime ↓", humanize_bytes(agg_rxt), pal::label,
        "lifetime ↑", humanize_bytes(agg_txt), pal::label,
        "", "", pal::dim));
    b.push_back(gap_row());

    // Per-interface breakdown. Sparks are peak-normalized per interface —
    // Spark clamps to [0,1], so raw B/s histories would render a solid wall.
    for (const NetIface& ni : s.nets) {
        auto rxn = norm48(ni.rx_history.data(), ni.hist_len);
        auto txn = norm48(ni.tx_history.data(), ni.hist_len);
        const double rxpk = hist_peak(ni.rx_history.data(), ni.hist_len);
        const double txpk = hist_peak(ni.tx_history.data(), ni.hist_len);
        b.push_back((h(
            text(fmt::clip(ni.name, 14)) | nowrap | Bold | fgc(pal::net_ac) | width(15),
            text(ni.up ? "● up" : "○ down") | nowrap | fgc(ni.up ? pal::good : pal::dim),
            Element{blank()} | grow(1),
            text("peak ▼ " + std::string(humanize_rate(ByteRate{rxpk}))
                 + "  ▲ " + std::string(humanize_rate(ByteRate{txpk})))
                | nowrap | fgc(pal::dim)
        ) | gap(1)).build());
        b.push_back((h(
            text("  ▼ rx") | nowrap | fgc(pal::sky) | width(7),
            Element{Spark{rxn.data(), ni.hist_len}.fill().color(pal::sky).baseline(true)} | grow(1),
            text(humanize_rate(ni.rx)) | nowrap | Bold | fgc(pal::sky) | width(12) | justify(Justify::End)
        ) | gap(1)).build());
        b.push_back((h(
            text("  ▲ tx") | nowrap | fgc(pal::good) | width(7),
            Element{Spark{txn.data(), ni.hist_len}.fill().color(pal::good).baseline(true)} | grow(1),
            text(humanize_rate(ni.tx)) | nowrap | Bold | fgc(pal::good) | width(12) | justify(Justify::End)
        ) | gap(1)).build());
        b.push_back(kv3(
            "  ↓ total", humanize_bytes(ni.rx_total), pal::label,
            "↑ total", humanize_bytes(ni.tx_total), pal::label,
            "", "", pal::dim));
        b.push_back(gap_row());
    }

    return b;
}

}  // namespace rockbottom::ui::detail
