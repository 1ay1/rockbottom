// widgets/detail/net.hpp — the NETWORK drill-down body.
//
// More than btop's single-interface graph: every interface at once, each with
// live rx/tx sparklines, the peak rate pulled from its own history, lifetime
// byte totals, and link state — plus an aggregate throughput line so you see
// the whole machine's traffic at a glance.
//
// WIDE layout is a real two-pane view: the interface roster sits STATIC on the
// left while the CONNECTIONS table scrolls INDEPENDENTLY on the right (its own
// window + scrollbar, driven by the pane scroll offset). The interfaces never
// drift as you page through 40 sockets — the two halves scroll on their own.

#pragma once

#include "common.hpp"

#include <maya/widget/table.hpp>

namespace rockbottom::ui::detail {

// Peak of a rolling history buffer — raw B/s samples.
inline double hist_peak(const float* h, int len) {
    double mx = 0;
    for (int i = 0; i < len; ++i) mx = std::max(mx, static_cast<double>(h[i]));
    return mx;
}

// Does the net pane render as two independent columns? (wide terminal + at
// least one socket to show on the right).
inline bool net_is_split(const Snapshot& s, const Ctx& cx) {
    return cx.wide && !s.connections.empty();
}

// ── connections table ────────────────────────────────────────────────────────────────────────
// The socket list is maya::Table — measured column plan (solve_columns),
// header rail, zebra stripes, span-exact … truncation, and (split mode)
// the host-scrolled window + scrollbar are all the framework's. This
// file supplies only the domain: which columns, their ink, and an owner
// cell built at the SOLVED width so the pid tail never falls off.

inline std::vector<maya::ColumnDef> conn_columns(bool with_proto) {
    using namespace maya;
    // The address pair never sheds; when the pane is tight PROTO goes
    // first, then STATE, then PROCESS. REMOTE (who you're talking to)
    // breathes the most, PROCESS next, LOCAL takes whatever is left once
    // the capped two stop growing — the same shares the old hand solver
    // dealt in percentages, now solved against measured floors instead
    // of guessed ones.
    std::vector<ColumnDef> cols;
    cols.push_back({.header = "", .width = 2, .keep = kKeepAlways});  // pane indent
    if (with_proto)
        cols.push_back({.header = "PROTO", .keep = 1});
    cols.push_back({.header = "LOCAL",   .keep = kKeepAlways,
                    .weight = 3.0f, .min_width = 14});
    cols.push_back({.header = "REMOTE",  .keep = kKeepAlways,
                    .weight = 3.8f, .min_width = 16, .max_width = 42});
    cols.push_back({.header = "STATE",   .keep = 2});
    cols.push_back({.header = "PROCESS", .keep = 3,
                    .weight = 3.2f, .min_width = 14, .max_width = 34});
    return cols;
}

inline maya::TableRow conn_table_row(const Connection& c, bool with_proto) {
    using namespace maya;
    const bool est = c.state == "ESTABLISHED";
    const bool lis = c.state == "LISTEN";
    const Color st_c = est ? pal::good : lis ? pal::sky
                     : c.state.empty() ? pal::dim : pal::hot;
    TableRow row;
    row.style = Style{}.with_fg(pal::label);   // base ink; spans override
    row.cells.emplace_back("");                // indent
    if (with_proto) row.cells.emplace_back(c.proto);
    row.cells.push_back(TableCell{}.span(c.laddr,
        Style{}.with_fg(est ? pal::text : pal::label)));
    row.cells.push_back(TableCell{}.span(c.raddr,
        Style{}.with_fg(est ? pal::sky : pal::dim)));
    row.cells.push_back(TableCell{}.span(c.state.empty() ? "\xc2\xb7" : c.state,
        Style{}.with_bold().with_fg(st_c)));
    // Owner: name + (pid), built AT the solved column width — the name
    // gives way first, the pid tail always survives. (The old code
    // clipped the name to a guessed budget; this one measures.)
    if (c.pid > 0) {
        row.cells.push_back(TableCell::dyn(
            [name = c.pname.empty() ? std::string{"?"} : c.pname,
             pid = c.pid](int w) -> TableCell {
                const std::string tail = " (" + std::to_string(pid) + ")";
                const int room = std::max(2, w - static_cast<int>(tail.size()));
                return {maya::truncate_end(name, room) + tail};
            }));
    } else {
        row.cells.emplace_back("\xe2\x80\x94");   // —
    }
    return row;
}

// One configured Table over the snapshot's socket list. Callers pick the
// windowing: the stacked pane FLOWS rows into its own element scroller
// (flow_rows), the split pane renders the block with visible_rows +
// window_top so the framework windows the body and draws the scrollbar.
inline maya::Table conn_table(const Snapshot& s, bool with_proto, int max_rows = 0) {
    using namespace maya;
    Table tbl(conn_columns(with_proto));
    auto& cfg = tbl.config();
    cfg.cell_padding   = 0;
    cfg.column_gap     = 1;
    cfg.show_separator = false;               // the header rail IS the rule
    cfg.header_style   = Style{}.with_bold().with_fg(mix(pal::net_ac, pal::text, 0.15));
    cfg.header_bg      = mix(pal::bg_panel, pal::net_ac, 0.14);
    cfg.stripe_rows    = true;                // zebra: quiet accent tint
    cfg.alt_row_style  = Style{}.with_bg(mix(pal::bg_panel, pal::net_ac, 0.06));
    cfg.scrollbar_thumb_color = pal::net_ac;
    cfg.scrollbar_track_color = pal::faint;
    const int total = static_cast<int>(s.connections.size());
    const int n = max_rows > 0 ? std::min(total, max_rows) : total;
    std::vector<TableRow> rows;
    rows.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        rows.push_back(conn_table_row(s.connections[static_cast<std::size_t>(i)], with_proto));
    tbl.set_rows(std::move(rows));
    return tbl;
}

inline std::string conn_summary(const Snapshot& s, int& established, int& listen) {
    established = 0; listen = 0;
    for (const auto& c : s.connections) {
        if (c.state == "ESTABLISHED") ++established;
        else if (c.state == "LISTEN") ++listen;
    }
    return std::to_string(established) + " active · " + std::to_string(listen) + " listening";
}

// How many socket rows the split-mode connections column can scroll past —
// the app clamps the pane scroll offset to this so ↑↓ never runs off the end.
// The right column shows a header + a window of `body_h - <band top>` rows.
inline int net_conn_scroll_max(const Snapshot& s, const Ctx& cx) {
    if (!net_is_split(s, cx)) return -1;   // not split → let the generic path clamp
    const int total = static_cast<int>(s.connections.size());
    // Rows above the table = hero traffic band (header + graph + gap) +
    // aggregate block (agg_rows) + the CONNECTIONS rule.
    const int hero_rows = std::max(4, cx.graph_h - 1) + 2;   // header + graph + gap
    const int agg_rows = 5;               // section + 2×kv3 + gap
    const int band_h = std::max(3, cx.body_h - hero_rows - agg_rows - 1);   // -1 for the rule
    const int table_view = std::max(1, band_h - 1);   // minus the header row
    return std::max(0, total - table_view);
}

// ── the pane body ─────────────────────────────────────────────────────────
inline std::vector<Element> net_body(const Snapshot& s, const Ctx& cx) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> b;

    if (s.nets.empty()) {
        b.push_back(verdict("no active interfaces — the machine is offline (or all links are down)", pal::dim));
        return b;
    }

    // Aggregate across interfaces first — the headline number. (Kept to a
    // stable row count so net_conn_scroll_max's agg_rows stays in sync.)
    double agg_rx = 0, agg_tx = 0;
    std::uint64_t agg_rxt = 0, agg_txt = 0;
    int up = 0;
    for (const NetIface& ni : s.nets) {
        agg_rx += ni.rx.per_sec; agg_tx += ni.tx.per_sec;
        agg_rxt += ni.rx_total.value; agg_txt += ni.tx_total.value;
        if (ni.up) ++up;
    }
    double agg_rpps = 0, agg_tpps = 0;
    std::uint64_t agg_errs = 0, agg_drops = 0;
    for (const NetIface& ni : s.nets) {
        agg_rpps += ni.rx_pps; agg_tpps += ni.tx_pps;
        agg_errs += ni.rx_errs + ni.tx_errs; agg_drops += ni.drops;
    }
    // Split geometry decided up-front so the aggregate header, the roster,
    // and the connections band all share ONE centered inner width — the
    // "fixed width, centered" chrome every other pane uses. `center()` wraps a
    // row in symmetric grow-spacer margins when split has surplus; otherwise
    // it's a transparent pass-through (narrow / non-split panes unchanged).
    const bool split = net_is_split(s, cx);
    constexpr int kBandDesign = 184;
    const int ngap    = 2;
    const int navail  = std::max(40, cx.w - 6);
    const int ninner  = split ? std::min(navail, kBandDesign) : navail;
    const int nside   = std::max(0, navail - ninner);
    const bool ncenter = split && nside >= 16;
    auto center = [&](Element e) -> Element {
        using namespace maya::dsl;
        if (!ncenter) return e;
        return (h(
            Element{blank()} | grow(1),
            std::move(e) | width(ninner),
            Element{blank()} | grow(1)
        )).build();
    };

    // ── hero: aggregate traffic over time (full-width, spans the pane) ─────
    // The house hero band: total download (filled) + upload (overlay line)
    // across ALL interfaces, on one shared peak so the two are comparable.
    // There's no aggregate history ring in the sampler, so sum every
    // interface's rx/tx history element-wise (the rings are index-aligned).
    {
        std::array<float, 48> arx{}, atx{};
        int hlen = 0;
        for (const NetIface& ni : s.nets) {
            hlen = std::max(hlen, ni.hist_len);
            for (int i = 0; i < ni.hist_len && i < 48; ++i) {
                arx[static_cast<std::size_t>(i)] += ni.rx_history[static_cast<std::size_t>(i)];
                atx[static_cast<std::size_t>(i)] += ni.tx_history[static_cast<std::size_t>(i)];
            }
        }
        float rpk = 1024.0f, tpk = 1024.0f;
        for (int i = 0; i < hlen && i < 48; ++i) {
            rpk = std::max(rpk, arx[static_cast<std::size_t>(i)]);
            tpk = std::max(tpk, atx[static_cast<std::size_t>(i)]);
        }
        const float shared_pk = std::max(rpk, tpk);
        std::array<float, 48> rn{}, tn{};
        for (int i = 0; i < hlen && i < 48; ++i) {
            rn[static_cast<std::size_t>(i)] = arx[static_cast<std::size_t>(i)] / shared_pk;
            tn[static_cast<std::size_t>(i)] = atx[static_cast<std::size_t>(i)] / shared_pk;
        }
        std::vector<Element> hdr;
        hdr.push_back(Element{center(section("TRAFFIC OVER TIME", pal::net_ac))} | grow(1));
        hdr.push_back((text("── down ") | nowrap | Bold | fgc(pal::sky)).build());
        hdr.push_back((text(" ── up ") | nowrap | Bold | fgc(pal::good)).build());
        b.push_back((h(std::move(hdr)) | gap(1)).build());
        const int gh = std::max(4, cx.graph_h - 1);
        b.push_back(center((h(
            y_axis(gh, static_cast<double>(shared_pk), 5, /*percent=*/false),
            Element{Graph{rn.data(), hlen}.fill().rows(gh).color(pal::sky)
                        .overlay(tn.data(), hlen, pal::good)} | grow(1)
        ) | gap(1) | height(gh)).build()));
        b.push_back(gap_row());
    }

    b.push_back(center(section("ALL INTERFACES", pal::net_ac)));
    b.push_back(center(kv3(
        "download", humanize_rate(ByteRate{agg_rx}), pal::sky,
        "upload", humanize_rate(ByteRate{agg_tx}), pal::good,
        "links up", std::to_string(up) + "/" + std::to_string(s.nets.size()),
        up > 0 ? pal::good : pal::dim)));
    b.push_back(center(kv3(
        "lifetime ↓", humanize_bytes(agg_rxt), pal::label,
        "lifetime ↑", humanize_bytes(agg_txt), pal::label,
        "packets", fmt::count(agg_rpps + agg_tpps) + "/s", pal::label)));
    // NOTE: net_conn_scroll_max assumes a FIXED aggregate height, so the
    // optional error line only shows in the STACKED (non-split) layout, where
    // scroll math counts rows directly.
    if ((agg_errs || agg_drops) && !split) {
        b.push_back(kv3(
            "errors", fmt::count(static_cast<double>(agg_errs)), agg_errs ? pal::hot : pal::dim,
            "dropped", fmt::count(static_cast<double>(agg_drops)), agg_drops ? pal::hot : pal::dim,
            "", "", pal::dim));
        if (agg_drops > 1000)
            b.push_back(verdict("▲ inbound packets are being dropped — a receiver is not keeping up", pal::hot));
    }
    b.push_back(gap_row());

    // ── per-interface roster (left column when split) ──
    std::vector<Element> ifcol;
    for (const NetIface& ni : s.nets) {
        auto rxn = norm48(ni.rx_history.data(), ni.hist_len);
        auto txn = norm48(ni.tx_history.data(), ni.hist_len);
        const double rxpk = hist_peak(ni.rx_history.data(), ni.hist_len);
        const double txpk = hist_peak(ni.tx_history.data(), ni.hist_len);
        ifcol.push_back(section(maya::truncate_end(ni.name, 14), pal::net_ac,
                            ni.up ? "● up" : "○ down"));
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
            ifcol.push_back(flow_row("  \xe2\x96\xbc rx", pal::sky, rxn.data(), ni.hist_len, pal::sky,
                std::string(humanize_rate(ni.rx)), pal::sky,
                {{"pk " + std::string(humanize_rate(ByteRate{rxpk})), pal::dim, 10}}));
            ifcol.push_back(flow_row("  \xe2\x96\xb2 tx", pal::good, txn.data(), ni.hist_len, pal::good,
                std::string(humanize_rate(ni.tx)), pal::good,
                {{"pk " + std::string(humanize_rate(ByteRate{txpk})), pal::dim, 10}}));
        } else {
            ifcol.push_back(flow_row("  \xe2\x96\xbc rx", pal::sky, rxn.data(), ni.hist_len, pal::sky,
                std::string(humanize_rate(ni.rx)), pal::sky,
                {// priority order: pps, then peak, then lifetime total
                 {fmt::count(ni.rx_pps) + " p/s", pal::dim, 8},
                 {"pk " + std::string(humanize_rate(ByteRate{rxpk})), pal::dim, 10},
                 {"\xe2\x86\x93 " + std::string(humanize_bytes(ni.rx_total)), pal::label, 8}}));
            ifcol.push_back(flow_row("  \xe2\x96\xb2 tx", pal::good, txn.data(), ni.hist_len, pal::good,
                std::string(humanize_rate(ni.tx)), pal::good,
                {{fmt::count(ni.tx_pps) + " p/s", pal::dim, 8},
                 {"pk " + std::string(humanize_rate(ByteRate{txpk})), pal::dim, 10},
                 {"\xe2\x86\x91 " + std::string(humanize_bytes(ni.tx_total)), pal::label, 8}}));
        }
        // Packet-rate + lifetime packet counts — a link can be saturated on
        // packets-per-second (tiny-packet floods, VoIP) while byte rate looks
        // idle, so surface pps and the lifetime counters as their own strip.
        if (ni.rx_pps > 0 || ni.tx_pps > 0 || ni.rx_packets || ni.tx_packets) {
            ifcol.push_back(kv3(
                "rx packets", fmt::count(ni.rx_pps) + "/s", pal::sky,
                "tx packets", fmt::count(ni.tx_pps) + "/s", pal::good,
                "lifetime pkts", fmt::count(static_cast<double>(ni.rx_packets + ni.tx_packets)),
                pal::label));
        }
        // Errors / drops broken out where the link is actually seeing them —
        // a healthy interface skips this row entirely, so it only appears as
        // a warning when there's something to warn about.
        if (ni.rx_errs || ni.tx_errs || ni.drops) {
            ifcol.push_back(kv3(
                "rx errors", fmt::count(static_cast<double>(ni.rx_errs)),
                ni.rx_errs ? pal::hot : pal::dim,
                "tx errors", fmt::count(static_cast<double>(ni.tx_errs)),
                ni.tx_errs ? pal::hot : pal::dim,
                "dropped in", fmt::count(static_cast<double>(ni.drops)),
                ni.drops ? pal::crit : pal::dim));
        }
        ifcol.push_back(gap_row());
    }

    // ── STACKED (narrow): interfaces then the full connections table ──
    if (!split) {
        // On an ultrawide terminal with no connections table to scroll (the
        // bespoke wide split above only fires when there ARE sockets), spend
        // the horizontal room by laying the interface roster out in two
        // side-by-side columns instead of one tall stack. `L`/`R` alias the
        // single vector in normal mode so everything stacks as before.
        const bool uw = cx.ultrawide && s.connections.empty() && s.nets.size() > 1;
        std::vector<Element> uw_left, uw_right;
        std::vector<Element>& L = uw ? uw_left : b;
        std::vector<Element>& R = uw ? uw_right : b;
        if (uw) {
            // Split the roster near the vertical midpoint: each interface owns
            // a fixed run of rows in ifcol, so cut on interface boundaries.
            const std::size_t half = (ifcol.size() + 1) / 2;
            for (std::size_t i = 0; i < ifcol.size(); ++i)
                (i < half ? L : R).push_back(std::move(ifcol[i]));
            return two_col(std::move(uw_left), std::move(uw_right));
        }
        for (auto& e : ifcol) b.push_back(std::move(e));
        if (!s.connections.empty()) {
            int est = 0, lis = 0;
            b.push_back(section("CONNECTIONS", pal::net_ac, conn_summary(s, est, lis)));
            // The pane scrolls element-by-element, so the table joins the
            // flow as one-row Elements sharing one solved plan (flow_rows).
            const int total = static_cast<int>(s.connections.size());
            const int shown = std::min(total, 40);
            for (auto& e : conn_table(s, /*with_proto=*/true, shown)
                               .flow_rows(std::max(40, cx.w - 4)))
                b.push_back(std::move(e));
            if (total > shown)
                b.push_back((text("   +" + std::to_string(total - shown) +
                                  " more sockets") | fgc(pal::dim)).build());
            b.push_back(gap_row());
        }
        return b;
    }

    // ── SPLIT (wide): interfaces STATIC left · connections SCROLL right ──
    // The whole band is a single fixed-height element, so the pane's outer
    // scroller renders it whole (no outer clipping) while the connections
    // column windows itself over the pane scroll offset — the two halves
    // scroll independently: paging sockets never moves the interfaces.
    const int hero_rows = std::max(4, cx.graph_h - 1) + 2;   // header + graph + gap
    const int agg_rows = 5;
    const int band_h   = std::max(3, cx.body_h - hero_rows - agg_rows - 1);   // -1 for CONNECTIONS rule

    // Geometry: roster capped ~52, the surplus to the denser socket table.
    // The band shares the SAME centered inner width as the aggregate header
    // (ninner / center() computed up-front) so the whole pane reads as one
    // fixed-width, centered slab.
    const int gap_w   = ngap;
    const int inner   = ninner;
    const int left_w  = std::clamp(52, 40, std::max(40, inner - gap_w - 40));
    const int right_w = inner - gap_w - left_w;

    // Left column: interface roster, top-aligned, padded to the band height so
    // the two columns are the same height. It does NOT scroll. Pack by
    // MEASURED rows — responsive rows (kv3 reflows to 2 lines at this column
    // width) count what they'll actually paint, so the roster can never grow
    // taller than the band and bleed into the hint bar.
    std::vector<Element> left;
    {
        int used = 0;
        for (auto& e : ifcol) {
            const int r = std::max(1, measure_element(e, left_w).height.value);
            if (used + r > band_h) break;
            used += r;
            left.push_back(std::move(e));
        }
        while (used < band_h) { left.push_back(gap_row()); ++used; }
    }

    // Right column: CONNECTIONS as a windowed maya::Table — the pane's
    // scroll offset drives window_top, the framework draws the ▍ thumb.
    int est = 0, lis = 0;
    const std::string summ = conn_summary(s, est, lis);
    const int table_view = std::max(1, band_h - 1);       // minus header row

    Table tbl = conn_table(s, /*with_proto=*/false);
    tbl.config().visible_rows = table_view;
    tbl.config().window_top   = cx.scroll;                // Table clamps

    // The CONNECTIONS section rule sits ABOVE the band (full right-column
    // width) so the header of what's scrolling is clearly labelled.
    std::vector<Element> right_stack;
    right_stack.push_back(section("CONNECTIONS", pal::net_ac, summ));
    right_stack.push_back(tbl.build());

    // Left column also gets a matching section-height offset so its first
    // interface rule lines up with the connections body, not the rule.
    std::vector<Element> left_stack;
    left_stack.push_back((text(" ") | nowrap).build());   // spacer to match the CONNECTIONS rule row
    left_stack.push_back((v(std::move(left))).build());

    Element band = (h(
        v(std::move(left_stack))  | width(left_w),
        v(std::move(right_stack)) | width(right_w)
    ) | gap(gap_w)).build();
    b.push_back(center(std::move(band)));

    return b;
}

}  // namespace rockbottom::ui::detail
