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
    double agg_rpps = 0, agg_tpps = 0;
    std::uint64_t agg_errs = 0, agg_drops = 0;
    for (const NetIface& ni : s.nets) {
        agg_rpps += ni.rx_pps; agg_tpps += ni.tx_pps;
        agg_errs += ni.rx_errs + ni.tx_errs; agg_drops += ni.drops;
    }
    b.push_back(kv3(
        "lifetime ↓", humanize_bytes(agg_rxt), pal::label,
        "lifetime ↑", humanize_bytes(agg_txt), pal::label,
        "packets", fmt::count(agg_rpps + agg_tpps) + "/s", pal::label));
    if (agg_errs || agg_drops) {
        b.push_back(kv3(
            "errors", fmt::count(static_cast<double>(agg_errs)), agg_errs ? pal::hot : pal::dim,
            "dropped", fmt::count(static_cast<double>(agg_drops)), agg_drops ? pal::hot : pal::dim,
            "", "", pal::dim));
        if (agg_drops > 1000)
            b.push_back(verdict("▲ inbound packets are being dropped — a receiver is not keeping up", pal::hot));
    }
    b.push_back(gap_row());

    // Per-interface breakdown. Each interface gets its own labelled section
    // rule — the same structural grammar as every other pane — with link
    // state riding the rule. Peak / lifetime figures live on the data rows
    // so every column aligns. Sparks are peak-normalized per interface
    // (Spark clamps to [0,1]; raw B/s renders a solid wall).
    //
    // WIDE layout: interfaces build into their own column so the CONNECTIONS
    // table can ride ALONGSIDE them instead of scrolling far below. Narrow
    // keeps the classic single stack.
    const bool split = cx.wide && !s.connections.empty();
    std::vector<Element> ifcol;   // per-interface rows (left column when split)

    // In split mode the per-interface figures lose the wide tail columns
    // (peak / lifetime) so the left column stays readable at ~half width.
    for (const NetIface& ni : s.nets) {
        auto rxn = norm48(ni.rx_history.data(), ni.hist_len);
        auto txn = norm48(ni.tx_history.data(), ni.hist_len);
        const double rxpk = hist_peak(ni.rx_history.data(), ni.hist_len);
        const double txpk = hist_peak(ni.tx_history.data(), ni.hist_len);
        ifcol.push_back(section(std::string(fmt::clip(ni.name, 14)), pal::net_ac,
                            ni.up ? "● up" : "○ down"));
        // Identity row: address / MAC / MTU — the "which network am I on"
        // facts every other monitor makes you shell out to ifconfig for.
        if (!ni.ip4.empty() || !ni.mac.empty()) {
            std::vector<Element> idr;
            idr.push_back((text("  ") | nowrap).build());
            if (!ni.ip4.empty()) {
                idr.push_back((text("ip ") | nowrap | fgc(pal::faint)).build());
                idr.push_back((text(ni.ip4) | nowrap | Bold | fgc(pal::sky)).build());
                idr.push_back((text("   ") | nowrap).build());
            }
            if (!ni.mac.empty()) {
                idr.push_back((text("mac ") | nowrap | fgc(pal::faint)).build());
                idr.push_back((text(ni.mac) | nowrap | fgc(pal::label)).build());
                idr.push_back((text("   ") | nowrap).build());
            }
            if (ni.mtu > 0) {
                idr.push_back((text("mtu ") | nowrap | fgc(pal::faint)).build());
                idr.push_back((text(std::to_string(ni.mtu)) | nowrap | fgc(pal::label)).build());
            }
            if (ni.rx_errs + ni.tx_errs > 0) {
                idr.push_back((text("   ") | nowrap).build());
                idr.push_back((text("errs ") | nowrap | fgc(pal::faint)).build());
                idr.push_back((text(fmt::count(static_cast<double>(ni.rx_errs + ni.tx_errs)))
                               | nowrap | Bold | fgc(pal::hot)).build());
            }
            if (ni.drops > 0) {
                idr.push_back((text("   ") | nowrap).build());
                idr.push_back((text("drop ") | nowrap | fgc(pal::faint)).build());
                idr.push_back((text(fmt::count(static_cast<double>(ni.drops)))
                               | nowrap | Bold | fgc(pal::hot)).build());
            }
            ifcol.push_back((h(std::move(idr))).build());
        }
        if (split) {
            // Compact rx/tx: spark + rate + peak only (drops p/s + lifetime).
            ifcol.push_back((h(
                text("  ▼ rx") | nowrap | fgc(pal::sky) | width(7),
                Element{Spark{rxn.data(), ni.hist_len}.fill().color(pal::sky).baseline(true)} | grow(1),
                text(humanize_rate(ni.rx)) | nowrap | Bold | fgc(pal::sky) | width(10) | justify(Justify::End),
                text("pk " + std::string(humanize_rate(ByteRate{rxpk}))) | nowrap | fgc(pal::dim) | width(12) | justify(Justify::End)
            ) | gap(1)).build());
            ifcol.push_back((h(
                text("  ▲ tx") | nowrap | fgc(pal::good) | width(7),
                Element{Spark{txn.data(), ni.hist_len}.fill().color(pal::good).baseline(true)} | grow(1),
                text(humanize_rate(ni.tx)) | nowrap | Bold | fgc(pal::good) | width(10) | justify(Justify::End),
                text("pk " + std::string(humanize_rate(ByteRate{txpk}))) | nowrap | fgc(pal::dim) | width(12) | justify(Justify::End)
            ) | gap(1)).build());
        } else {
            ifcol.push_back((h(
                text("  ▼ rx") | nowrap | fgc(pal::sky) | width(7),
                Element{Spark{rxn.data(), ni.hist_len}.fill().color(pal::sky).baseline(true)} | grow(1),
                text(humanize_rate(ni.rx)) | nowrap | Bold | fgc(pal::sky) | width(10) | justify(Justify::End),
                text(fmt::count(ni.rx_pps) + " p/s") | nowrap | fgc(pal::dim) | width(10) | justify(Justify::End),
                text("pk " + std::string(humanize_rate(ByteRate{rxpk}))) | nowrap | fgc(pal::dim) | width(12) | justify(Justify::End),
                text("↓ " + std::string(humanize_bytes(ni.rx_total))) | nowrap | fgc(pal::label) | width(9) | justify(Justify::End)
            ) | gap(1)).build());
            ifcol.push_back((h(
                text("  ▲ tx") | nowrap | fgc(pal::good) | width(7),
                Element{Spark{txn.data(), ni.hist_len}.fill().color(pal::good).baseline(true)} | grow(1),
                text(humanize_rate(ni.tx)) | nowrap | Bold | fgc(pal::good) | width(10) | justify(Justify::End),
                text(fmt::count(ni.tx_pps) + " p/s") | nowrap | fgc(pal::dim) | width(10) | justify(Justify::End),
                text("pk " + std::string(humanize_rate(ByteRate{txpk}))) | nowrap | fgc(pal::dim) | width(12) | justify(Justify::End),
                text("↑ " + std::string(humanize_bytes(ni.tx_total))) | nowrap | fgc(pal::label) | width(9) | justify(Justify::End)
            ) | gap(1)).build());
        }
        ifcol.push_back(gap_row());
    }

    // Stacked layout appends the interface column straight into the body;
    // split layout holds it for the side-by-side compose below.
    if (!split)
        for (auto& e : ifcol) b.push_back(std::move(e));

    // ── connections ───────────────────────────────────────────────
    // Who is talking to whom — the ss / lsof -i / nethogs answer, attributed to
    // owning processes. Established connections first, then listeners. This is
    // the network view every process monitor should have and almost none do.
    std::vector<Element> concol;
    std::vector<Element>& cc = split ? concol : b;
    if (!s.connections.empty()) {
        int established = 0, listen = 0;
        for (const auto& c : s.connections) {
            if (c.state == "ESTABLISHED") ++established;
            else if (c.state == "LISTEN") ++listen;
        }
        cc.push_back(section("CONNECTIONS", pal::net_ac,
                            std::to_string(established) + " active · " +
                            std::to_string(listen) + " listening"));

        // Column widths. Full-width (stacked) shows the roomy 5-column table;
        // the split right column drops `proto` and tightens local/remote so
        // "who am I talking to" still reads at ~half screen.
        const int la_w = split ? 18 : 22;
        const int ra_w = split ? 18 : 22;
        const int st_w = split ? 11 : 13;

        // Column header for the socket table.
        std::vector<Element> hdr;
        if (!split) hdr.push_back((text("  proto") | nowrap | fgc(pal::faint) | width(8)).build());
        else        hdr.push_back((text("  ") | nowrap | width(2)).build());
        hdr.push_back((text("local") | nowrap | fgc(pal::faint) | width(la_w)).build());
        hdr.push_back((text("remote") | nowrap | fgc(pal::faint) | width(ra_w)).build());
        hdr.push_back((text("state") | nowrap | fgc(pal::faint) | width(st_w)).build());
        hdr.push_back((text("process") | nowrap | fgc(pal::faint) | grow(1)).build());
        cc.push_back((h(std::move(hdr)) | gap(1)).build());

        const int shown = std::min<int>(static_cast<int>(s.connections.size()),
                                        cx.wide ? 40 : 24);
        for (int i = 0; i < shown; ++i) {
            const Connection& c = s.connections[static_cast<std::size_t>(i)];
            const bool est = c.state == "ESTABLISHED";
            const bool lis = c.state == "LISTEN";
            Color st_c = est ? pal::good : lis ? pal::sky
                       : c.state.empty() ? pal::dim : pal::hot;
            std::string who = c.pid > 0
                ? std::string(fmt::clip(c.pname.empty() ? "?" : c.pname, split ? 12 : 20)) +
                  " (" + std::to_string(c.pid) + ")"
                : "—";
            std::vector<Element> row;
            if (!split) row.push_back((text("  " + c.proto) | nowrap | fgc(pal::label) | width(8)).build());
            else        row.push_back((text("  ") | nowrap | width(2)).build());
            row.push_back((text(fmt::clip(c.laddr, static_cast<std::size_t>(la_w - 1))) | nowrap | fgc(est ? pal::text : pal::label) | width(la_w)).build());
            row.push_back((text(fmt::clip(c.raddr, static_cast<std::size_t>(ra_w - 1))) | nowrap | fgc(est ? pal::sky : pal::dim) | width(ra_w)).build());
            row.push_back((text(c.state.empty() ? "·" : c.state) | nowrap | Bold | fgc(st_c) | width(st_w)).build());
            row.push_back((text(who) | nowrap | fgc(pal::label) | grow(1)).build());
            cc.push_back((h(std::move(row)) | gap(1)).build());
        }
        if (static_cast<int>(s.connections.size()) > shown)
            cc.push_back((text("   +" + std::to_string(static_cast<int>(s.connections.size()) - shown) +
                              " more sockets") | fgc(pal::dim)).build());
        cc.push_back(gap_row());
    }

    // Compose: side-by-side in wide mode. The scroller slices the body vector
    // by ELEMENT (one entry == one screen row), so we can't hand it a single
    // tall two-column block — that would render unclipped and overflow the
    // frame. Instead we ZIP the columns row-for-row into 1-row-tall pairs,
    // padding the shorter column with blanks, so scroll math stays exact.
    if (split) {
        const int gap_w   = 2;
        const int inner   = std::max(40, cx.w - 6);      // pane chrome slack
        const int left_w  = std::clamp((inner - gap_w) * 52 / 100, 34, inner - gap_w - 30);
        const int right_w = inner - gap_w - left_w;
        const std::size_t n = std::max(ifcol.size(), concol.size());
        for (std::size_t i = 0; i < n; ++i) {
            Element l = i < ifcol.size() ? std::move(ifcol[i]) : gap_row();
            Element r = i < concol.size() ? std::move(concol[i]) : gap_row();
            b.push_back((h(
                std::move(l) | width(left_w),
                std::move(r) | width(right_w)
            ) | gap(gap_w)).build());
        }
    }

    return b;
}

}  // namespace rockbottom::ui::detail
