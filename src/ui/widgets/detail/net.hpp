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

// ── connections table ────────────────────────────────────────────────────
// A tidy monospace table: proto · local → remote · state chip · owner. Column
// widths are computed from the available width so the row FILLS its column and
// the state chip / owner never float away from the addresses. `avail` is the
// width the table gets; `with_proto` shows the proto column (stacked layout)
// vs a 2-space indent (split, where width is tighter).
struct ConnCols { int indent, la, ra, st, proc; };

inline ConnCols conn_cols(int avail, bool with_proto) {
    ConnCols c;
    c.indent = with_proto ? 8 : 2;                 // "  proto" vs bare indent
    c.st     = with_proto ? 12 : 11;
    const int gaps = 4;                            // inter-column gap(1) ×4
    const int rest = std::max(30, avail - c.indent - c.st - gaps);
    // remote (who you're talking to) gets the most, then owner, then local.
    c.ra   = std::clamp(rest * 38 / 100, 16, 42);
    c.proc = std::clamp(rest * 32 / 100, 14, 34);
    c.la   = std::max(14, rest - c.ra - c.proc);
    return c;
}

// A single formatted socket row. `zebra` tints alternate rows for readability.
inline Element conn_row(const Connection& c, const ConnCols& cols, bool with_proto,
                        bool zebra) {
    using namespace maya; using namespace maya::dsl;
    const bool est = c.state == "ESTABLISHED";
    const bool lis = c.state == "LISTEN";
    const Color st_c = est ? pal::good : lis ? pal::sky
                     : c.state.empty() ? pal::dim : pal::hot;
    const Color la_c = est ? pal::text : pal::label;
    const Color ra_c = est ? pal::sky  : pal::dim;
    // Owner: name + (pid). The name is clipped to the owner column minus the
    // "(12345)" tail so the pid always stays visible.
    const int name_room = std::max(6, cols.proc - 8);
    std::string who = c.pid > 0
        ? std::string(fmt::clip(c.pname.empty() ? "?" : c.pname,
                                static_cast<std::size_t>(name_room))) +
          " (" + std::to_string(c.pid) + ")"
        : "—";
    std::string state = c.state.empty() ? "·" : c.state;

    std::vector<Element> row;
    if (with_proto)
        row.push_back((text("  " + c.proto) | nowrap | fgc(pal::label) | width(cols.indent)).build());
    else
        row.push_back((text("  ") | nowrap | width(cols.indent)).build());
    row.push_back((text(std::string(fmt::clip(c.laddr, static_cast<std::size_t>(cols.la - 1))))
                   | nowrap | fgc(la_c) | width(cols.la)).build());
    row.push_back((text(std::string(fmt::clip(c.raddr, static_cast<std::size_t>(cols.ra - 1))))
                   | nowrap | fgc(ra_c) | width(cols.ra)).build());
    row.push_back((text(state) | nowrap | Bold | fgc(st_c) | width(cols.st)).build());
    row.push_back((text(who) | nowrap | fgc(pal::label) | grow(1)).build());
    Element line = (h(std::move(row)) | gap(1)).build();
    if (zebra) line = std::move(line) | bgc(mix(pal::bg_panel, pal::net_ac, 0.06));
    return line.build();
}

inline Element conn_header(const ConnCols& cols, bool with_proto) {
    using namespace maya; using namespace maya::dsl;
    // A real table header: bold, accent-tinted labels sitting on a subtle
    // header bar so the column titles read as a heading, not another data row.
    const Color hc  = mix(pal::net_ac, pal::text, 0.15);   // bright accent label
    const Color bar = mix(pal::bg_panel, pal::net_ac, 0.14); // header-row tint
    auto col = [&](const char* label, int w, bool grow_it = false) {
        auto e = text(label) | nowrap | Bold | fgc(hc);
        return grow_it ? (std::move(e) | grow(1)).build()
                       : (std::move(e) | width(w)).build();
    };
    std::vector<Element> hdr;
    if (with_proto)
        hdr.push_back(col("PROTO", cols.indent));
    else
        hdr.push_back((text("  ") | nowrap | width(cols.indent)).build());
    hdr.push_back(col("LOCAL", cols.la));
    hdr.push_back(col("REMOTE", cols.ra));
    hdr.push_back(col("STATE", cols.st));
    hdr.push_back(col("PROCESS", 0, /*grow_it=*/true));
    return ((h(std::move(hdr)) | gap(1)) | bgc(bar)).build();
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
    // Rows above the table = aggregate block (agg_rows) + the CONNECTIONS rule.
    const int agg_rows = 5;               // section + 2×kv3 + gap
    const int band_h = std::max(3, cx.body_h - agg_rows - 1);   // -1 for the rule
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
    b.push_back(section("ALL INTERFACES", pal::net_ac));
    b.push_back(kv3(
        "download", humanize_rate(ByteRate{agg_rx}), pal::sky,
        "upload", humanize_rate(ByteRate{agg_tx}), pal::good,
        "links up", std::to_string(up) + "/" + std::to_string(s.nets.size()),
        up > 0 ? pal::good : pal::dim));
    b.push_back(kv3(
        "lifetime ↓", humanize_bytes(agg_rxt), pal::label,
        "lifetime ↑", humanize_bytes(agg_txt), pal::label,
        "packets", fmt::count(agg_rpps + agg_tpps) + "/s", pal::label));
    // NOTE: net_conn_scroll_max assumes a FIXED aggregate height, so the
    // optional error line only shows in the STACKED (non-split) layout, where
    // scroll math counts rows directly.
    const bool split = net_is_split(s, cx);
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
        ifcol.push_back(section(std::string(fmt::clip(ni.name, 14)), pal::net_ac,
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
            const ConnCols cols = conn_cols(std::max(40, cx.w - 4), /*with_proto=*/true);
            b.push_back(conn_header(cols, true));
            const int shown = std::min<int>(static_cast<int>(s.connections.size()), 40);
            for (int i = 0; i < shown; ++i)
                b.push_back(conn_row(s.connections[static_cast<std::size_t>(i)], cols, true, i & 1));
            if (static_cast<int>(s.connections.size()) > shown)
                b.push_back((text("   +" + std::to_string(static_cast<int>(s.connections.size()) - shown) +
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
    const int agg_rows = 5;
    const int band_h   = std::max(3, cx.body_h - agg_rows - 1);   // -1 for CONNECTIONS rule

    // Geometry: roster capped ~52, the surplus to the denser socket table.
    const int gap_w   = 2;
    const int inner   = std::max(40, cx.w - 6);
    const int left_w  = std::clamp(52, 40, std::max(40, inner - gap_w - 40));
    const int right_w = inner - gap_w - left_w;

    // Left column: interface roster, top-aligned, padded to the band height so
    // the two columns are the same height. It does NOT scroll.
    std::vector<Element> left = std::move(ifcol);
    while (static_cast<int>(left.size()) < band_h) left.push_back(gap_row());
    if (static_cast<int>(left.size()) > band_h) left.resize(static_cast<std::size_t>(band_h));

    // Right column: CONNECTIONS with its own header + windowed body + scrollbar.
    int est = 0, lis = 0;
    const std::string summ = conn_summary(s, est, lis);
    const ConnCols cols = conn_cols(right_w - 1, /*with_proto=*/false);  // -1 for scrollbar

    const int total = static_cast<int>(s.connections.size());
    const int table_view = std::max(1, band_h - 1);       // minus header row
    const int max_scroll = std::max(0, total - table_view);
    const int scroll = std::clamp(cx.scroll, 0, max_scroll);

    std::vector<Element> tbody;
    tbody.push_back(conn_header(cols, false));
    for (int i = scroll; i < scroll + table_view && i < total; ++i)
        tbody.push_back(conn_row(s.connections[static_cast<std::size_t>(i)], cols, false, i & 1));
    while (static_cast<int>(tbody.size()) < band_h) tbody.push_back(gap_row());

    // Scrollbar for the connections column (matches the pane scroller idiom).
    Element right_col;
    if (total > table_view) {
        const int thumb = std::max(1, table_view * table_view / total);
        const int track = table_view - thumb;
        const int pos   = max_scroll > 0 ? scroll * track / max_scroll : 0;
        std::vector<Element> bar;
        bar.push_back((text(" ") | nowrap).build());     // align with header row
        for (int r = 0; r < table_view; ++r) {
            const bool on = r >= pos && r < pos + thumb;
            const char* g = r == 0 && scroll > 0 ? "▲"
                          : r == table_view - 1 && scroll < max_scroll ? "▼"
                          : on ? "█" : "│";
            bar.push_back((text(g) | nowrap | fgc(on ? pal::net_ac : pal::faint)).build());
        }
        while (static_cast<int>(bar.size()) < band_h) bar.push_back((text(" ") | nowrap).build());
        right_col = (h(
            v(std::move(tbody)) | grow(1),
            v(std::move(bar)) | width(1)
        )).build();
    } else {
        right_col = (v(std::move(tbody))).build();
    }

    // The CONNECTIONS section rule sits ABOVE the band (full right-column
    // width) so the header of what's scrolling is clearly labelled.
    std::vector<Element> right_stack;
    right_stack.push_back(section("CONNECTIONS", pal::net_ac, summ));
    right_stack.push_back(std::move(right_col));

    // Left column also gets a matching section-height offset so its first
    // interface rule lines up with the connections body, not the rule.
    std::vector<Element> left_stack;
    left_stack.push_back((text(" ") | nowrap).build());   // spacer to match the CONNECTIONS rule row
    left_stack.push_back((v(std::move(left))).build());

    b.push_back((h(
        v(std::move(left_stack))  | width(left_w),
        v(std::move(right_stack)) | width(right_w)
    ) | gap(gap_w)).build());

    return b;
}

}  // namespace rockbottom::ui::detail
