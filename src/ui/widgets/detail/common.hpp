// widgets/detail/common.hpp — shared building blocks for every detail pane.
//
// The detail panes are bottom's "go deeper" surface: full-screen, per-domain
// drill-downs that carry MORE detail than htop or btop. Every number gets room
// to breathe, real graphs get a y-axis, and plain-language verdicts read the
// numbers *for* you. This header holds the primitives each per-domain file
// composes from — plus the RESPONSIVE context and the SCROLLER that keeps
// dense panes usable at any terminal size.

#pragma once

#include <maya/maya.hpp>

#include "../../../core/metrics.hpp"
#include "../../fmt.hpp"
#include "../../theme.hpp"
#include "../panel.hpp"
#include "../graph.hpp"
#include "../spark.hpp"
#include "../meter.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace rockbottom::ui::detail {

using maya::Element;

// Responsive context handed to every pane. `w`/`h` are the *terminal* size;
// panes use them to drop columns, shrink graphs, and pick densities. `scroll`
// is the body row offset (managed by the app). `body_h` is how many content
// rows fit inside the framed viewport (== h - chrome).
struct Ctx {
    int  w = 100;
    int  h = 40;
    int  scroll = 0;
    // Derived responsive flags, filled by make().
    bool wide = true;      // room for multi-column / side-by-side layouts
    bool tall = true;      // room for the big graphs
    int  graph_h = 8;      // rows available for the hero graph
    int  body_h = 30;      // scrollable viewport height

    static Ctx make(int w, int h, int scroll) {
        Ctx c;
        c.w = w; c.h = h; c.scroll = std::max(0, scroll);
        c.wide = w >= 84;
        c.tall = h >= 30;
        // Frame chrome: panel border(2) + panel padding(2) + hint(1) = 5 rows.
        c.body_h = std::max(3, h - 5);
        c.graph_h = std::clamp(h - 20, 4, 12);
        return c;
    }
};

// ── content primitives ───────────────────────────────────────────────────

// A label : value row — label dim + fixed width, value bold + colored.
inline Element kv(const std::string& k, const std::string& v, maya::Color vc,
                  int kw = 16) {
    using namespace maya; using namespace maya::dsl;
    return (h(
        text(k) | nowrap | fgc(pal::dim) | width(kw),
        text(v) | nowrap | Bold | fgc(vc)
    )).build();
}

// Up to three label:value pairs on one line — dense stat strips. Empty key =
// blank spacer cell (lets callers show 1 or 2 pairs too).
inline Element kv3(const std::string& k1, const std::string& v1, maya::Color c1,
                   const std::string& k2 = "", const std::string& v2 = "", maya::Color c2 = pal::dim,
                   const std::string& k3 = "", const std::string& v3 = "", maya::Color c3 = pal::dim) {
    using namespace maya; using namespace maya::dsl;
    auto cell = [](const std::string& k, const std::string& v, maya::Color c) -> Element {
        using namespace maya; using namespace maya::dsl;
        if (k.empty()) return (Element{blank()} | grow(1)).build();
        return (h(
            text(k) | nowrap | fgc(pal::dim),
            text(" ") | nowrap,
            text(v) | nowrap | Bold | fgc(c)
        ) | grow(1)).build();
    };
    return (h(cell(k1, v1, c1), cell(k2, v2, c2), cell(k3, v3, c3)) | gap(2)).build();
}

// A full-width labelled meter row: "label  ██████░░  42%  <tail>".
// `tail_w` shrinks on narrow terminals (caller passes ctx.wide ? 34 : 0).
inline Element bar(const std::string& label, double frac,
                   const std::string& tail, maya::Color c, int tail_w = 34) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> row;
    row.push_back((text(label) | nowrap | fgc(pal::label) | width(14)).build());
    row.push_back((Element{Meter{frac}.fill().color(c)} | grow(1)).build());
    row.push_back((text(fmt::pct_pad(frac)) | nowrap | Bold | fgc(load_color(frac)) | width(5) | justify(Justify::End)).build());
    if (tail_w > 0)
        row.push_back((text(tail) | nowrap | fgc(pal::dim) | width(tail_w)).build());
    return (h(std::move(row)) | gap(2)).build();
}

// A section heading in the domain accent color.
inline Element section(const char* title, maya::Color ac) {
    using namespace maya; using namespace maya::dsl;
    return (text(title) | nowrap | Bold | fgc(ac)).build();
}

// A plain-language verdict line — bottom's signature "read it for you" touch.
inline Element verdict(const std::string& msg, maya::Color c) {
    using namespace maya; using namespace maya::dsl;
    return (text("  " + msg) | nowrap | fgc(c)).build();
}

// A blank spacer row.
inline Element gap_row() {
    using namespace maya; using namespace maya::dsl;
    return blank();
}

// ── SCROLLER ───────────────────────────────────────────────────────────────
// Take a full body (may be taller than the viewport) and return a window of at
// most `view_h` rows starting at `scroll`, with a slim right-edge scrollbar and
// top/bottom "more" chevrons so it's obvious there's content off-screen. This
// is what makes every pane usable no matter how dense or how small the term.
inline Element scroller(std::vector<Element> body, int scroll, int view_h,
                        maya::Color ac) {
    using namespace maya; using namespace maya::dsl;
    const int total = static_cast<int>(body.size());
    if (total <= view_h) {
        // Fits — no scrollbar needed, just pad to fill so the frame is stable.
        return (v(std::move(body)) | grow(1)).build();
    }
    const int max_scroll = total - view_h;
    scroll = std::clamp(scroll, 0, max_scroll);

    std::vector<Element> win;
    for (int i = scroll; i < scroll + view_h && i < total; ++i)
        win.push_back(std::move(body[static_cast<std::size_t>(i)]));

    // Scrollbar thumb: proportional, at least 1 row.
    const int thumb = std::max(1, view_h * view_h / total);
    const int track = view_h - thumb;
    const int pos = max_scroll > 0 ? scroll * track / max_scroll : 0;
    std::vector<Element> barcol;
    for (int r = 0; r < view_h; ++r) {
        const bool on = r >= pos && r < pos + thumb;
        const char* g = r == 0 && scroll > 0 ? "▲"
                      : r == view_h - 1 && scroll < max_scroll ? "▼"
                      : on ? "█" : "│";
        barcol.push_back((text(g) | nowrap | fgc(on ? ac : pal::faint)).build());
    }

    return (h(
        v(std::move(win)) | grow(1),
        text(" ") | nowrap,
        v(std::move(barcol)) | width(1)
    )).build();
}

// Total content rows for a body — used by the app to clamp scroll offsets.
// (Panes expose their body length via the DetailPane::content_rows API.)

}  // namespace rockbottom::ui::detail
