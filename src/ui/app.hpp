// app.hpp — the rockbottom Program: Elm-style Model/Msg/update/view wiring.
// All rendering is delegated to the ui::* widgets; all data collection to
// core::Sampler. This file is just the state machine and layout composition.
//
// Interaction model (all keyboard, zero modes to memorize):
//   ↑/↓ or j/k   move the process selection
//   /            filter processes by name (Esc clears)
//   x / Delete   ask to end the selected process (SIGTERM)
//   K            ask to force-kill (SIGKILL)
//   y / Enter    confirm pending kill · n / Esc cancels
//   s c m n P    sorting · space pause · ? help · q quit

#pragma once

#include <maya/maya.hpp>

#include "../core/sampler.hpp"
#include "state.hpp"
#include "theme.hpp"
#include "widgets/header.hpp"
#include "widgets/verdict.hpp"
#include "widgets/cpu_panel.hpp"
#include "widgets/mem_panel.hpp"
#include "widgets/disk_panel.hpp"
#include "widgets/net_panel.hpp"
#include "widgets/proc_panel.hpp"
#include "widgets/footer.hpp"
#include "widgets/help.hpp"
#include "widgets/detail.hpp"

#include <algorithm>
#include <csignal>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rockbottom {

struct App {
    struct Model {
        Snapshot snap;
        SortKey  sort = SortKey::Cpu;
        bool     paused = false;
        bool     show_help = false;
        ui::Detail detail = ui::Detail::None;   // full-screen drill-down
        int      detail_scroll = 0;              // scroll offset within a pane
        int      width = 100, height = 40;
        int      ticks = 0;

        // Process interaction state.
        int         sel = 0;             // index into the *filtered* view
        std::string filter;              // active name filter ("" = off)
        bool        filtering = false;   // '/' typing mode
        std::optional<PendingKill> pending;
        std::optional<Toast>       toast;

        std::shared_ptr<Sampler> sampler = std::make_shared<Sampler>();
    };

    struct Tick {};
    struct Resize { int w, h; };
    struct Key { maya::KeyEvent ev; };
    struct Mouse { maya::MouseEvent ev; };
    struct Quit {};

    using Msg = std::variant<Tick, Resize, Key, Mouse, Quit>;

    // ── shared layout geometry ──────────────────────────────────────────────
    // The mouse handler and view() MUST agree on where every panel lands, so
    // the vertical/height arithmetic lives here once and both call it. Any
    // divergence would make clicks land on the wrong row — this keeps misses
    // at exactly zero because there is a single source of truth.
    struct Layout {
        bool narrow;
        int  top_h;          // rows spanned by the CPU / MEM-NET-DISK band
        int  proc_rows;      // ProcView.max_rows
        int  body_rows;      // visible process rows in the table
        int  graph_h, graph_w, cpu_cols;
        int  right_w, left_w;
        // Absolute (0-based) terminal rows. Outer padding is (0,1,0,1): zero
        // vertical padding, so the vstack starts at row 0.
        int  header_y   = 0;                 // Header widget
        int  verdict_y  = 1;                 // VerdictBanner (3 rows: 1..3)
        int  top_y      = 4;                 // top band first row
        int  proc_top_y;                     // proc panel top border row
        int  proc_hdr_y;                     // column-header row (PID USER …)
        int  proc_body_y;                    // first process row
        int  footer_y;                       // footer row (== height-1)
    };

    static Layout compute_layout(const Model& m) {
        const Snapshot& s = m.snap;
        Layout L;
        L.narrow = m.width < 96;
        const int ncores = static_cast<int>(s.cpu.cores.size());
        L.cpu_cols = ncores > 24 ? 4 : ncores > 12 ? 3 : 2;
        const int cores_rows = (ncores + L.cpu_cols - 1) / L.cpu_cols;
        const int mem_h  = 2 + (s.mem.swap_total.value > 0 ? 2 : 1);
        const int net_h  = 2 + std::max(1, static_cast<int>(s.nets.size()));
        const int disk_mounts = static_cast<int>(s.disks.size());
        const int disk_h = 2 + 1 + disk_mounts;
        const int right_stack_h = mem_h + net_h + disk_h;
        if (L.narrow) {
            const int fixed = 2 + 3 + 1 + 2 + 1 + cores_rows + right_stack_h + (2 + 5) + 2;
            L.graph_h = std::clamp(m.height - fixed, 0, 4);
        } else {
            L.graph_h = std::clamp(right_stack_h - 3 - cores_rows, 2, 8);
        }
        const int cpu_h = 2 + 1 + (L.graph_h >= 2 ? L.graph_h : 1) + cores_rows;
        L.top_h = L.narrow ? cpu_h + mem_h + net_h + disk_h
                           : std::max(cpu_h, right_stack_h);
        L.proc_rows = std::max(5, m.height - 5 - L.top_h - 2);
        L.body_rows = std::max(3, L.proc_rows - 1);

        const int inner = std::max(20, m.width - 2);
        const int gap_w = 1;
        L.right_w = std::clamp((inner - gap_w) * 42 / 100, 34, 56);
        L.left_w  = inner - gap_w - L.right_w;
        const int cpu_inner = (L.narrow ? inner : L.left_w) - 4;
        L.graph_w = std::max(8, cpu_inner - 4);

        // Anchor the proc-table rows from the BOTTOM, not the top. The 1-row
        // Header widget gets squeezed out of the v-stack when height is tight,
        // which would shift a top-anchored table by one row and mis-route every
        // click. The v-stack is top-aligned and leaves ONE trailing blank row,
        // so the footer lands at height-2 and the proc panel directly above it.
        // Counting up from the panel bottom border is exact at any height.
        L.footer_y    = m.height - 2;
        const int proc_bot_border = L.footer_y - 1;     // panel bottom border
        const int proc_body_last  = proc_bot_border - 1;// last visible row
        L.proc_body_y = proc_body_last - (L.body_rows - 1);
        L.proc_hdr_y  = L.proc_body_y - 1;              // column-header row
        L.proc_top_y  = L.proc_hdr_y - 1;               // panel top border
        L.top_y       = 4;   // nominal; not used for hit-testing
        return L;
    }

    // First visible process index given the scroll window (mirrors
    // ProcPanel::build()'s clamp exactly so click→index is never off).
    static int scroll_start(int sel, int n, int body_rows) {
        int start = 0;
        if (sel >= body_rows) start = sel - body_rows + 1;
        return std::clamp(start, 0, std::max(0, n - body_rows));
    }

    // ── footer hit-testing ─────────────────────────────────────────────
    enum class FooterAct { Quit, Filter, End, Kill, Sort, Pause, Help };

    // Rebuild the SAME hint sequence Footer renders, tracking each label's
    // column span so a click resolves to the right action. Only the normal-mode
    // strip is clickable (filter/pending modes are keyboard-driven).
    // Layout mirror of Footer::build(): outer padding adds 1 leading column;
    // each hint is " "+k + "·"+d; hints are joined with gap(1).
    static std::optional<FooterAct> footer_hit(const Model& m, int mx) {
        if (m.filtering || m.pending) return std::nullopt;
        struct H { const char* k; const char* d; FooterAct a; };
        static const H hints[] = {
            {"q", "quit",   FooterAct::Quit},
            {"\u2191\u2193", "select", FooterAct::End /*label only, no action*/},
            {"/", "filter", FooterAct::Filter},
            {"x", "end",    FooterAct::End},
            {"K", "kill",   FooterAct::Kill},
            {"s", "sort",   FooterAct::Sort},
            {"1-6", "detail", FooterAct::End /*label only, no action*/},
            {"space", "pause", FooterAct::Pause},
            {"?", "help",   FooterAct::Help},
        };
        int col = 1;   // outer left padding
        for (std::size_t i = 0; i < std::size(hints); ++i) {
            const int kw = disp_width(hints[i].k);
            const int dw = disp_width(hints[i].d);
            const int w  = 1 /*leading space*/ + kw + 1 /*·*/ + dw;
            if (mx >= col && mx < col + w) {
                // ↑↓ select and 1-6 detail are labels with no click action.
                if (std::string(hints[i].k) == "\u2191\u2193" ||
                    std::string(hints[i].k) == "1-6") return std::nullopt;
                return hints[i].a;
            }
            col += w + 1;   // + gap(1)
        }
        return std::nullopt;
    }

    static std::pair<Model, maya::Cmd<Msg>> dispatch_footer(Model m, FooterAct a) {
        using C = maya::Cmd<Msg>;
        switch (a) {
            case FooterAct::Quit:  return {std::move(m), C::quit()};
            case FooterAct::Filter: m.filtering = true; m.filter.clear(); m.sel = 0; return {std::move(m), C{}};
            case FooterAct::End:   return arm_kill(std::move(m), SIGTERM);
            case FooterAct::Kill:  return arm_kill(std::move(m), SIGKILL);
            case FooterAct::Sort:  m.sort = static_cast<SortKey>((static_cast<int>(m.sort) + 1) % 6); return resample(std::move(m));
            case FooterAct::Pause: m.paused = !m.paused; return {std::move(m), C{}};
            case FooterAct::Help:  m.show_help = true; return {std::move(m), C{}};
        }
        return {std::move(m), C{}};
    }

    // ── sort-header hit-testing ───────────────────────────────────────
    // The proc panel sits under outer padding(1) + panel border(1) + panel
    // padding(1) = 3 leading columns before the header content. We map a click
    // in the header row to the column it lands on. Rather than reproduce every
    // column's exact width, we split the header row into three coarse zones
    // that are forgiving yet unambiguous: left third of the numeric block
    // cycles nothing, but each visible sort label owns a zone. To stay simple
    // and miss-free we test against the SAME width tiers the header uses.
    static std::optional<SortKey> sort_header_hit(const Model& m, int mx) {
        const int w = std::max(20, m.width - 6);   // ProcView.width
        const bool show_port = w >= 92;
        const bool show_memp = w >= 62;
        const bool show_mem  = w >= 54;
        const bool show_io   = w >= 84;
        const bool show_thr  = w >= 70;
        // Leading offset: outer pad(1) + border(1) + panel pad(1) = 3, but the
        // rendered labels sit one cell left of the raw column math, so anchor
        // at 2 and make every zone CONTIGUOUS (each swallows its trailing gap)
        // so there are no dead columns between headers — a click anywhere in the
        // header row resolves to exactly one column.
        int c = 2;
        auto zone = [&](int width) { int s = c; c += width + 1; return std::pair{s, c}; };
        zone(8);   // PID  (no sort)
        zone(8);   // USER (no sort)
        int fixed = 8 + 8;                                  // PID + USER
        if (show_port) fixed += 9 + 1;
        fixed += (show_mem ? 14 : 8) + 1 + 6;               // CPU meter + gap + value
        fixed += 8;                                          // MEM
        if (show_memp) fixed += 5 + 1;
        if (show_io)   fixed += 8 + 1;
        fixed += 2;                                          // S
        if (show_thr)  fixed += 4 + 1;
        const int name_w = std::max(4, w - fixed - 2);
        auto [name_s, name_e] = zone(name_w);
        if (mx >= name_s && mx < name_e) return SortKey::Name;
        if (show_port) { auto [s,e] = zone(9); if (mx >= s && mx < e) return SortKey::Port; }
        { auto [s,e] = zone((show_mem ? 14 : 8) + 1 + 6); if (mx >= s && mx < e) return SortKey::Cpu; }
        { auto [s,e] = zone(8); if (mx >= s && mx < e) return SortKey::Mem; }
        if (show_memp) { auto [s,e] = zone(5); if (mx >= s && mx < e) return SortKey::Mem; }
        if (show_io)   { auto [s,e] = zone(8); if (mx >= s && mx < e) return SortKey::Io; }
        return std::nullopt;
    }

    // Display width of a UTF-8 label (wide box/arrow glyphs count as 1 cell in
    // this app's font set except the ↑↓ pair which is two 1-cell glyphs).
    static int disp_width(const char* s) {
        int w = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ) {
            unsigned char c = *p;
            int len = (c < 0x80) ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xe ? 3 : 4;
            w += 1;   // every codepoint here is one terminal cell
            p += len;
        }
        return w;
    }

    // ── mouse handling ───────────────────────────────────────────────────────
    static std::pair<Model, maya::Cmd<Msg>> on_mouse(Model m, const maya::MouseEvent& me) {
        using C = maya::Cmd<Msg>;
        using maya::MouseButton;
        using maya::MouseEventKind;

        // maya delivers 1-based cell coords; convert to 0-based frame rows/cols.
        const int mx = me.x.value - 1;
        const int my = me.y.value - 1;

        const Layout L = compute_layout(m);
        auto view = filtered(m);
        const int n = static_cast<int>(view.size());

        const bool over_table =
            my >= L.proc_body_y && my < L.proc_body_y + L.body_rows;

        // ── Scroll wheel ──
        // In a scrollable detail pane the wheel moves the pane window; over the
        // process table it moves the selection; anywhere else it still scrolls
        // the list so the wheel is never a dead input.
        if (me.button == MouseButton::ScrollDown) {
            if (m.detail != ui::Detail::None && m.detail != ui::Detail::Proc) {
                m.detail_scroll += 3; clamp_detail_scroll(m); return {std::move(m), C{}};
            }
            m.sel += 3; clamp_sel(m); return {std::move(m), C{}};
        }
        if (me.button == MouseButton::ScrollUp) {
            if (m.detail != ui::Detail::None && m.detail != ui::Detail::Proc) {
                m.detail_scroll -= 3; clamp_detail_scroll(m); return {std::move(m), C{}};
            }
            m.sel -= 3; clamp_sel(m); return {std::move(m), C{}};
        }

        // Only act on button presses for the rest (ignore Move/Release so we
        // don't double-fire; drags fall through harmlessly).
        if (me.kind != MouseEventKind::Press) return {std::move(m), C{}};

        // Modal layers first — a click outside the modal dismisses it.
        if (m.show_help) { m.show_help = false; return {std::move(m), C{}}; }
        if (m.detail != ui::Detail::None) {
            // In the detail overlay the wheel already scrolled the selection;
            // a left click on the bottom hint row switches domain, anything
            // else closes the pane.
            m.detail = ui::Detail::None;
            return {std::move(m), C{}};
        }
        if (m.pending) {
            // Footer shows y·confirm / n·cancel; a click on the table row of the
            // pending process (or anywhere) just cancels for safety — killing is
            // keyboard-confirmed only, never a stray click.
            m.pending.reset();
            return {std::move(m), C{}};
        }

        // ── Footer hint clicks ──
        if (my == L.footer_y && me.button == MouseButton::Left) {
            if (auto act = footer_hit(m, mx)) return dispatch_footer(std::move(m), *act);
            return {std::move(m), C{}};
        }

        // ── Sort-header clicks (the column-header row of the proc table) ──
        if (my == L.proc_hdr_y && me.button == MouseButton::Left && !m.filtering) {
            if (auto sk = sort_header_hit(m, mx)) {
                m.sort = *sk;
                return resample(std::move(m));
            }
            return {std::move(m), C{}};
        }

        // ── Top-band panel clicks → open that domain's detail ──
        // The band sits directly above the proc panel. Left column = CPU, right
        // column (split at left_w) stacks MEM / NET / DISK. In narrow mode it's
        // a single column CPU→MEM→NET→DISK.
        const int band_top = L.proc_top_y - L.top_h;   // first band row
        if (my >= band_top && my < L.proc_top_y && me.button == MouseButton::Left) {
            if (L.narrow) {
                const Snapshot& s = m.snap;
                const int ncores = static_cast<int>(s.cpu.cores.size());
                const int cores_rows = (ncores + L.cpu_cols - 1) / L.cpu_cols;
                const int cpu_h = 2 + 1 + (L.graph_h >= 2 ? L.graph_h : 1) + cores_rows;
                const int mem_h = 2 + (s.mem.swap_total.value > 0 ? 2 : 1);
                const int net_h = 2 + std::max(1, static_cast<int>(s.nets.size()));
                if (my < band_top + cpu_h)                 m.detail = ui::Detail::Cpu;
                else if (my < band_top + cpu_h + mem_h)    m.detail = ui::Detail::Mem;
                else if (my < band_top + cpu_h + mem_h + net_h) m.detail = ui::Detail::Net;
                else                                       m.detail = ui::Detail::Disk;
            } else {
                const int inner_left = 1 + L.left_w;   // outer pad + CPU column
                if (mx <= inner_left) {
                    m.detail = ui::Detail::Cpu;
                } else {
                    // Right stack: MEM (top), NET (mid), DISK (bottom) by row.
                    const Snapshot& s = m.snap;
                    const int mem_h = 2 + (s.mem.swap_total.value > 0 ? 2 : 1);
                    const int net_h = 2 + std::max(1, static_cast<int>(s.nets.size()));
                    const int ry = my - band_top;
                    if (ry < mem_h)              m.detail = ui::Detail::Mem;
                    else if (ry < mem_h + net_h) m.detail = ui::Detail::Net;
                    else                         m.detail = ui::Detail::Disk;
                }
            }
            return {std::move(m), C{}};
        }

        // ── Process row clicks ──
        if (over_table && n > 0) {
            const int start = scroll_start(m.sel, n, L.body_rows);
            const int row   = my - L.proc_body_y;      // 0-based visible row
            const int idx   = start + row;
            if (idx >= 0 && idx < n) {
                if (me.button == MouseButton::Left) {
                    m.sel = idx;
                } else if (me.button == MouseButton::Right) {
                    // Right-click a row = arm a SIGTERM on it (keyboard confirms).
                    m.sel = idx;
                    return arm_kill(std::move(m), SIGTERM);
                }
            }
            return {std::move(m), C{}};
        }

        return {std::move(m), C{}};
    }

    // ── init / update ───────────────────────────────────────────────────────

    static std::pair<Model, maya::Cmd<Msg>> init() {
        Model m;
        m.snap = m.sampler->sample(m.sort, 400);
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    static std::pair<Model, maya::Cmd<Msg>> update(Model m, Msg msg) {
        using C = maya::Cmd<Msg>;
        return std::visit(maya::overload{
            [&](Tick) {
                if (!m.paused) { ++m.ticks; m.snap = m.sampler->sample(m.sort, 400); }
                if (m.toast && --m.toast->ttl <= 0) m.toast.reset();
                clamp_sel(m);
                return std::pair{std::move(m), C{}};
            },
            [&](Resize r) { m.width = r.w; m.height = r.h; return std::pair{std::move(m), C{}}; },
            [&](Key k)    { return on_key(std::move(m), k.ev); },
            [&](Mouse mo) { return on_mouse(std::move(m), mo.ev); },
            [&](Quit)     { return std::pair{std::move(m), C::quit()}; },
        }, msg);
    }

    // ── key handling: one place, mode-aware ─────────────────────────────────

    static std::pair<Model, maya::Cmd<Msg>> on_key(Model m, const maya::KeyEvent& ke) {
        using C = maya::Cmd<Msg>;
        maya::Event ev{ke};
        using maya::key;

        // 1. Kill confirmation intercepts everything.
        if (m.pending) {
            if (key(ev, 'y') || key(ev, maya::SpecialKey::Enter)) {
                std::string err = signal_process(m.pending->pid, m.pending->sig);
                std::string verb = m.pending->sig == SIGKILL ? "force-killed " : "asked ";
                std::string tail = m.pending->sig == SIGKILL ? "" : " to exit";
                m.toast = err.empty()
                    ? Toast{verb + m.pending->name + " (" + std::to_string(m.pending->pid) + ")" + tail, false}
                    : Toast{err, true};
                m.pending.reset();
                m.snap = m.sampler->sample(m.sort, 400);
            } else if (key(ev, 'n') || key(ev, maya::SpecialKey::Escape) || key(ev, 'q')) {
                m.pending.reset();
            }
            return {std::move(m), C{}};
        }

        // 2. Filter typing mode.
        if (m.filtering) {
            if (key(ev, maya::SpecialKey::Escape)) { m.filtering = false; m.filter.clear(); }
            else if (key(ev, maya::SpecialKey::Enter)) { m.filtering = false; }
            else if (key(ev, maya::SpecialKey::Backspace)) {
                if (!m.filter.empty()) m.filter.pop_back();
            } else if (auto* ck = std::get_if<maya::CharKey>(&ke.key);
                       ck && ck->codepoint >= 0x20 && ck->codepoint < 0x7f) {
                m.filter += static_cast<char>(ck->codepoint);
            }
            m.sel = 0;
            return {std::move(m), C{}};
        }

        // 3. Help overlay: any of these dismiss.
        if (m.show_help) {
            if (key(ev, '?') || key(ev, 'h') || key(ev, maya::SpecialKey::Escape) || key(ev, 'q'))
                m.show_help = false;
            return {std::move(m), C{}};
        }

        // 3b. Detail pane (full-screen drill-down): Esc/q/Enter close it, the
        // number keys switch domain, and in the process view x/K still work.
        if (m.detail != ui::Detail::None) {
            if (key(ev, maya::SpecialKey::Escape) || key(ev, 'q') ||
                key(ev, maya::SpecialKey::Enter)) {
                m.detail = ui::Detail::None; m.detail_scroll = 0; return {std::move(m), C{}};
            }
            if (key(ev, '1')) { m.detail = ui::Detail::Cpu;  m.detail_scroll = 0; return {std::move(m), C{}}; }
            if (key(ev, '2')) { m.detail = ui::Detail::Mem;  m.detail_scroll = 0; return {std::move(m), C{}}; }
            if (key(ev, '3')) { m.detail = ui::Detail::Net;  m.detail_scroll = 0; return {std::move(m), C{}}; }
            if (key(ev, '4')) { m.detail = ui::Detail::Gpu;  m.detail_scroll = 0; return {std::move(m), C{}}; }
            if (key(ev, '5')) { m.detail = ui::Detail::Disk; m.detail_scroll = 0; return {std::move(m), C{}}; }
            if (key(ev, '6')) { m.detail = ui::Detail::Proc; m.detail_scroll = 0; return {std::move(m), C{}}; }
            if (m.detail == ui::Detail::Proc) {
                // In the process pane ↑↓ walk the selection (and the app keeps
                // it visible); other keys still work.
                if (key(ev, maya::SpecialKey::Down) || key(ev, 'j')) { ++m.sel; clamp_sel(m); return {std::move(m), C{}}; }
                if (key(ev, maya::SpecialKey::Up)   || key(ev, 'k')) { --m.sel; clamp_sel(m); return {std::move(m), C{}}; }
                if (key(ev, 'x') || key(ev, maya::SpecialKey::Delete)) return arm_kill(std::move(m), SIGTERM);
                if (key(ev, 'K')) return arm_kill(std::move(m), SIGKILL);
            } else {
                // Every other pane is scrollable with the usual keys.
                if (key(ev, maya::SpecialKey::Down) || key(ev, 'j')) { m.detail_scroll += 1; clamp_detail_scroll(m); return {std::move(m), C{}}; }
                if (key(ev, maya::SpecialKey::Up)   || key(ev, 'k')) { m.detail_scroll -= 1; clamp_detail_scroll(m); return {std::move(m), C{}}; }
                if (key(ev, maya::SpecialKey::PageDown) || key(ev, ' ')) { m.detail_scroll += 10; clamp_detail_scroll(m); return {std::move(m), C{}}; }
                if (key(ev, maya::SpecialKey::PageUp))  { m.detail_scroll -= 10; clamp_detail_scroll(m); return {std::move(m), C{}}; }
                if (key(ev, maya::SpecialKey::Home) || key(ev, 'g')) { m.detail_scroll = 0; return {std::move(m), C{}}; }
                if (key(ev, maya::SpecialKey::End)  || key(ev, 'G')) { m.detail_scroll = 1 << 20; clamp_detail_scroll(m); return {std::move(m), C{}}; }
            }
            return {std::move(m), C{}};
        }

        // 4. Normal mode.
        if (key(ev, 'q') || key(ev, maya::SpecialKey::Escape)) {
            if (!m.filter.empty()) { m.filter.clear(); m.sel = 0; return {std::move(m), C{}}; }
            return {std::move(m), C::quit()};
        }
        if (key(ev, 'p') || key(ev, ' '))  { m.paused = !m.paused; return {std::move(m), C{}}; }
        if (key(ev, '?') || key(ev, 'h'))  { m.show_help = true; return {std::move(m), C{}}; }
        if (key(ev, '/'))                  { m.filtering = true; m.filter.clear(); m.sel = 0; return {std::move(m), C{}}; }

        // Detail drill-down: 1-5 open a full-screen domain view; Enter opens
        // the selected process's detail.
        if (key(ev, '1')) { m.detail = ui::Detail::Cpu;  m.detail_scroll = 0; return {std::move(m), C{}}; }
        if (key(ev, '2')) { m.detail = ui::Detail::Mem;  m.detail_scroll = 0; return {std::move(m), C{}}; }
        if (key(ev, '3')) { m.detail = ui::Detail::Net;  m.detail_scroll = 0; return {std::move(m), C{}}; }
        if (key(ev, '4')) { m.detail = ui::Detail::Gpu;  m.detail_scroll = 0; return {std::move(m), C{}}; }
        if (key(ev, '5')) { m.detail = ui::Detail::Disk; m.detail_scroll = 0; return {std::move(m), C{}}; }
        if (key(ev, '6') || key(ev, maya::SpecialKey::Enter)) { m.detail = ui::Detail::Proc; m.detail_scroll = 0; return {std::move(m), C{}}; }

        if (key(ev, 's')) { m.sort = static_cast<SortKey>((static_cast<int>(m.sort) + 1) % 6); return resample(std::move(m)); }
        if (key(ev, 'c')) { m.sort = SortKey::Cpu;  return resample(std::move(m)); }
        if (key(ev, 'm')) { m.sort = SortKey::Mem;  return resample(std::move(m)); }
        if (key(ev, 'i')) { m.sort = SortKey::Io;   return resample(std::move(m)); }
        if (key(ev, 'P')) { m.sort = SortKey::Pid;  return resample(std::move(m)); }
        if (key(ev, 'n')) { m.sort = SortKey::Name; return resample(std::move(m)); }
        if (key(ev, 'o')) { m.sort = SortKey::Port; return resample(std::move(m)); }

        // Selection.
        if (key(ev, maya::SpecialKey::Down) || key(ev, 'j')) { ++m.sel; clamp_sel(m); return {std::move(m), C{}}; }
        if (key(ev, maya::SpecialKey::Up)   || key(ev, 'k')) { --m.sel; clamp_sel(m); return {std::move(m), C{}}; }
        if (key(ev, maya::SpecialKey::Home)) { m.sel = 0; return {std::move(m), C{}}; }
        if (key(ev, maya::SpecialKey::PageDown)) { m.sel += 10; clamp_sel(m); return {std::move(m), C{}}; }
        if (key(ev, maya::SpecialKey::PageUp))   { m.sel -= 10; clamp_sel(m); return {std::move(m), C{}}; }

        // Kill.
        if (key(ev, 'x') || key(ev, maya::SpecialKey::Delete)) return arm_kill(std::move(m), SIGTERM);
        if (key(ev, 'K'))                                      return arm_kill(std::move(m), SIGKILL);

        return {std::move(m), C{}};
    }

    // ── helpers ─────────────────────────────────────────────────────────────

    static std::vector<const ProcInfo*> filtered(const Model& m) {
        std::vector<const ProcInfo*> out;
        for (const auto& p : m.snap.procs) {
            if (m.filter.empty() ||
                p.name.find(m.filter) != std::string::npos ||
                std::to_string(p.pid).find(m.filter) != std::string::npos)
                out.push_back(&p);
        }
        return out;
    }

    static void clamp_sel(Model& m) {
        int n = static_cast<int>(filtered(m).size());
        m.sel = std::clamp(m.sel, 0, std::max(0, n - 1));
    }

    // Clamp the detail-pane scroll offset to [0, content - viewport]. Builds a
    // throwaway DetailPane to ask it how tall its body is at the current size.
    static void clamp_detail_scroll(Model& m) {
        const ProcInfo* p = nullptr;
        if (m.detail == ui::Detail::Proc) {
            auto view = filtered(m);
            if (!view.empty() && m.sel < static_cast<int>(view.size()))
                p = view[static_cast<std::size_t>(m.sel)];
        }
        ui::DetailPane pane{m.snap, m.detail, p, m.width, m.height, 0};
        int max_scroll = std::max(0, pane.content_rows() - pane.viewport_rows());
        m.detail_scroll = std::clamp(m.detail_scroll, 0, max_scroll);
    }

    static std::pair<Model, maya::Cmd<Msg>> arm_kill(Model m, int sig) {
        auto view = filtered(m);
        if (!view.empty() && m.sel < static_cast<int>(view.size())) {
            const ProcInfo* p = view[static_cast<std::size_t>(m.sel)];
            m.pending = PendingKill{p->pid, p->name, sig};
        }
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    static std::pair<Model, maya::Cmd<Msg>> resample(Model m) {
        m.snap = m.sampler->sample(m.sort, 400);
        clamp_sel(m);
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // ── subscriptions ───────────────────────────────────────────────────────

    static maya::Sub<Msg> subscribe(const Model&) {
        using S = maya::Sub<Msg>;
        using namespace std::chrono_literals;
        std::vector<S> subs;
        subs.push_back(S::every(1s, Tick{}));
        subs.push_back(S::on_resize([](maya::Size sz) -> Msg {
            return Resize{sz.width.value, sz.height.value};
        }));
        subs.push_back(S::on_key([](const maya::KeyEvent& ke) -> std::optional<Msg> {
            return Key{ke};
        }));
        subs.push_back(S::on_mouse([](const maya::MouseEvent& me) -> std::optional<Msg> {
            return Mouse{me};
        }));
        return S::batch(std::move(subs));
    }

    // ── view: pure composition of widgets ───────────────────────────────────

    static maya::Element view(const Model& m) {
        using namespace maya;
        using namespace maya::dsl;
        using namespace rockbottom::ui;

        if (m.show_help) return HelpOverlay{};

        if (m.detail != ui::Detail::None) {
            const ProcInfo* p = nullptr;
            if (m.detail == ui::Detail::Proc) {
                auto view = filtered(m);
                if (!view.empty() && m.sel < static_cast<int>(view.size()))
                    p = view[static_cast<std::size_t>(m.sel)];
            }
            return DetailPane{m.snap, m.detail, p, m.width, m.height, m.detail_scroll};
        }

        const Snapshot& s = m.snap;
        const bool narrow = m.width < 96;

        // ── Height budget ──
        // Fixed rows: header(1) + verdict(3) + footer(1) + outer padding.
        // Top band: CPU panel (graph 4 + blank + cores) vs MEM+NET+DISK stack.
        const int ncores = static_cast<int>(s.cpu.cores.size());
        const int cpu_cols = ncores > 24 ? 4 : ncores > 12 ? 3 : 2;   // stay ~8 rows tall
        const int cores_rows = (ncores + cpu_cols - 1) / cpu_cols;
        const int mem_h  = 2 + (s.mem.swap_total.value > 0 ? 2 : 1);
        const int net_h  = 2 + std::max(1, static_cast<int>(s.nets.size()));
        const int disk_mounts = static_cast<int>(s.disks.size());
        const int disk_h = 2 + 1 + disk_mounts;                       // one mount per row on the right

        // The ALL graph is the first thing to shrink when height is scarce.
        // Wide mode: match the CPU column's height to the MEM+NET+DISK stack so
        // neither column leaves a trailing gap; the graph flexes to fit.
        // Narrow mode: everything stacks, so fit the graph into what is left
        // after the other cards + a 5-row process table minimum.
        const int right_stack_h = mem_h + net_h + disk_h;
        int graph_h = 4;
        if (narrow) {
            const int fixed = 2 + 3 + 1                 // header+verdict+footer
                            + 2 + 1 + cores_rows        // cpu border + ALL row + cores
                            + right_stack_h
                            + (2 + 5)                   // proc border + 5 rows
                            + 2;                        // outer padding slack
            graph_h = std::clamp(m.height - fixed, 0, 4);
        } else {
            // cpu_h = 2(border) + 1(ALL header) + graph_h + cores_rows. Solve
            // for the graph_h that makes cpu_h == right_stack_h, clamped sane.
            graph_h = std::clamp(right_stack_h - 3 - cores_rows, 2, 8);
        }
        const int cpu_h  = 2 + 1 + (graph_h >= 2 ? graph_h : 1) + cores_rows;
        const int top_h  = narrow ? cpu_h + mem_h + net_h + disk_h
                                  : std::max(cpu_h, right_stack_h);
        const int proc_rows = std::max(5, m.height - 5 - top_h - 2);

        ProcView pv{
            .procs     = filtered(m),
            .sort      = m.sort,
            .selected  = m.sel,
            .max_rows  = proc_rows,
            .width     = std::max(20, m.width - 6),
            .filter    = m.filter,
            .filtering = m.filtering,
            .pending   = m.pending ? &*m.pending : nullptr,
        };

        // ── Top band: CPU alone on the left · MEM / NET / DISK stacked right ──
        // Split the inner width so neither side starves the other; the CPU
        // graph wants the wider slice, the stat cards the rest.
        const int inner = std::max(20, m.width - 2);      // minus outer padding
        const int gap_w = 1;
        const int right_w = std::clamp((inner - gap_w) * 42 / 100, 34, 56);
        const int left_w  = inner - gap_w - right_w;
        // Inner content width of a panel = box width - 2 border - 2 padding.
        const int cpu_inner = (narrow ? inner : left_w) - 4;
        const int graph_w = std::max(8, cpu_inner - 4);   // minus y-axis (3) + gap (1)
        Element top = narrow
            ? (v(CpuPanel{s.cpu, cpu_cols, graph_w, graph_h}, MemPanel{s.mem}, NetPanel{s.nets},
                 DiskPanel{s.disks, s.disk_io, false})).build()
            : (h(
                  Element{CpuPanel{s.cpu, cpu_cols, graph_w, graph_h}} | width(left_w),
                  v(MemPanel{s.mem}, NetPanel{s.nets},
                    DiskPanel{s.disks, s.disk_io, false}) | width(right_w)
              ) | gap(gap_w)).build();

        return (v(
            Header{s, m.paused},
            VerdictBanner{s},
            std::move(top),
            ProcPanel{s, pv},
            Footer{m.paused, m.ticks, m.toast ? &*m.toast : nullptr,
                   m.pending ? &*m.pending : nullptr, m.filtering, m.filter}
        ) | padding(0, 1, 0, 1)).build();
    }
};

}  // namespace rockbottom
