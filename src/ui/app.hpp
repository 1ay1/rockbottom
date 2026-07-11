// app.hpp — the rockbottom Program: Elm-style Model/Msg/update/view wiring.
// All rendering is delegated to the ui::* widgets; all data collection to
// core::Sampler. This file is just the state machine and layout composition.
//
// Interaction model (all keyboard, zero modes to memorize):
//   ↑/↓ or j/k   move the process selection
//   /            filter processes by name (Esc clears)
//   t            toggle process tree · ←/→ collapse/expand · * follow
//   x / Delete   ask to end the selected process (SIGTERM)
//   K            ask to force-kill (SIGKILL)
//   l            open the signal picker (send ANY signal)
//   r            renice — change scheduling priority
//   y / Enter    confirm pending kill · n / Esc cancels
//   s c m n P    sorting (re-press to reverse) · space pause · ? help · q quit

#pragma once

#include <maya/maya.hpp>

#include "../core/sampler.hpp"
#include "../core/config.hpp"
#include "state.hpp"
#include "proc_order.hpp"
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
#include "widgets/signal_menu.hpp"
#include "widgets/nice_menu.hpp"
#include "widgets/detail.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace rockbottom {

struct App {
    // Boot configuration (CLI flags overlaid on the persisted file), set by
    // main() before maya::run<App>. init() seeds the Model from it, and a clean
    // quit writes the current view state back so the tool reopens as you left
    // it. A function-local static keeps it out of a translation-unit global.
    static Config& boot_config() { static Config c; return c; }

    struct Model {
        Snapshot snap;
        SortKey  sort = SortKey::Cpu;
        bool     paused = false;
        int      refresh_ms = 1000;              // sample cadence; < > adjust (250–5000)
        bool     show_help = false;
        int      help_scroll = 0;                // scroll offset within help
        ui::Detail detail = ui::Detail::None;   // full-screen drill-down
        int      detail_scroll = 0;              // scroll offset within a pane
        int      detail_pid = 0;                 // PID the Proc pane is pinned to
        int      width = 100, height = 40;
        int      ticks = 0;
        // Monotonic generation bumped every time a new Snapshot is folded in
        // (a Sampled message). visual_hash() folds this instead of deep-
        // hashing the whole snapshot: a new sample = new data = new frame,
        // and nothing else mutates snap.
        std::uint64_t snap_gen = 0;

        // Process interaction state.
        int         sel = 0;             // index into the *filtered* view
        int         scroll_top = 0;      // first visible row of the proc table
                                         // (sticky: moves only when the cursor
                                         //  would cross the scroll margin)
        std::string filter;              // active name filter ("" = off)
        bool        filtering = false;   // '/' typing mode
        bool     sort_desc = true;    // sort direction (▼ default, ▲ reversed)
        bool        tree = false;         // flat sorted list (DEFAULT) vs process tree
        bool        auto_folded = false;  // (unused with expand-default tree; kept for compat)
        std::set<int> collapsed;         // pids whose subtree is folded (tree mode)
        int         follow_pid = 0;      // keep this pid selected as the list moves (* toggles)
        std::optional<PendingKill> pending;
        std::optional<Toast>       toast;

        // Signal picker (htop's F9 menu): when set, an overlay lists every
        // signal in the catalog and the number keys / ↑↓ pick one. Choosing
        // arms a PendingKill with that signal, so the same y/n confirm and
        // group semantics apply to ANY signal, not just TERM/KILL.
        struct SigMenu {
            int  anchor_pid = 0;      // process the menu was opened on
            std::string name;         // its name (for the confirm strip)
            std::vector<int> pids;    // targets (group-aware, like PendingKill)
            int  sel = 0;             // index into signal_catalog()
        };
        std::optional<SigMenu> sigmenu;

        // Renice picker: adjust a process's nice value (scheduling priority).
        // Unlike the signal menu this applies on Enter via setpriority(2).
        struct NiceMenu {
            int  pid = 0;
            std::string name;
            int  cur = 0;    // current nice, for reference
            int  val = 0;    // the value being dialed in
        };
        std::optional<NiceMenu> nicemenu;

        // Verdict pulse: when health DEGRADES the banner border flares and
        // fades over the next few ticks so the state change catches the eye.
        Health last_health = Health::Calm;
        int    verdict_pulse = 0;   // ticks of flare remaining

        // True while a background sample is in flight. The Sampler holds
        // cross-tick delta state (CPU busy fractions, net/proc rates), so only
        // ONE sample may run at a time — this guard drops Ticks that arrive
        // while a slow sample (e.g. a wedged nvidia-smi) is still running,
        // instead of queuing a backlog on the worker thread.
        bool        sampling = false;

        std::shared_ptr<Sampler> sampler = std::make_shared<Sampler>();

        // Memoized ordered view. order_procs() (filter query + sort + tree +
        // rollups) is the single most expensive UI computation — ~4us flat,
        // ~40us filtered, ~116us tree — and it was recomputed on EVERY call to
        // ordered()/filtered()/selected_pid()/clamp_sel() plus once per render,
        // i.e. several times per frame. We cache the last result keyed by a
        // hash of its inputs (snap_gen, sort, dir, tree, filter, collapsed set),
        // so a frame computes it AT MOST ONCE. A shared_ptr keeps the cache
        // alive across the Model's moves through update() and costs nothing to
        // copy. `mutable` because ordered() takes a const Model& everywhere.
        struct OrderedCache { std::uint64_t key = ~0ULL; ui::OrderedProcs value; };
        mutable std::shared_ptr<OrderedCache> ord_cache =
            std::make_shared<OrderedCache>();
    };

    struct Tick {};
    struct Resize { int w, h; };
    struct Key { maya::KeyEvent ev; };
    struct Mouse { maya::MouseEvent ev; };
    struct Sampled { Snapshot snap; };   // a background sample finished
    struct Quit {};

    using Msg = std::variant<Tick, Resize, Key, Mouse, Sampled, Quit>;

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
        // Wide-2col stacks the CPU card in a narrow left column: 2-3 core
        // columns (mirror view()). Non-wide keeps the classic packing.
        const bool wide_screen = m.width >= 200;
        L.cpu_cols = wide_screen ? (ncores > 16 ? 3 : 2)
                                 : (ncores > 24 ? 4 : ncores > 12 ? 3 : 2);
        // Narrow mode packs the cores into a single heat-strip row.
        const int cores_rows = L.narrow ? 1 : (ncores + L.cpu_cols - 1) / L.cpu_cols;
        const int mem_h  = 2 + (s.mem.swap_total.value > 0 ? 2 : 1);
        const int net_h  = 2 + ui::NetPanel::rows(s.nets);
        const int disk_mounts = static_cast<int>(s.disks.size());
        const int disk_h = 2 + 1 + disk_mounts;
        const int right_stack_h = mem_h + net_h + disk_h;
        if (L.narrow) {
            const int fixed = 2 + 3 + 1 + 2 + 1 + cores_rows + right_stack_h + (2 + 5) + 2;
            L.graph_h = std::clamp(m.height - fixed, 0, 12);
        } else {
            // Mirror view(): the classic top band fills the space above a
            // readable process table, so the CPU graph (and thus cpu_h) grows
            // to that height and the right column fills the same band 50/50.
            const int chrome = 2 + 3 + 1 + 2;
            const int proc_min = 12;
            const int avail = m.height - chrome - proc_min;
            const int want = std::max(avail, right_stack_h);
            L.graph_h = std::clamp(want - 3 - cores_rows, 2, 22);
        }
        const int cpu_h = 2 + 1 + (L.graph_h >= 2 ? L.graph_h : 1) + cores_rows;
        L.top_h = L.narrow ? cpu_h + mem_h + net_h + disk_h
                           : std::max(cpu_h, right_stack_h);
        // Wide 2-col: the process table runs the full band height beside the
        // stacked stat column, so its row count is the band, not a strip under
        // the top band. Must mirror view()'s wide2 math or sync_scroll drifts.
        const bool wide2 = m.width >= 200;
        const int band_h = std::max(6, m.height - 5);
        L.proc_rows = wide2 ? std::max(5, band_h - 2)
                            : std::max(5, m.height - 5 - L.top_h - 2);
        L.body_rows = std::max(3, L.proc_rows - 1);

        const int inner = std::max(20, m.width - 2);
        const int gap_w = 1;
        // Non-wide: classic CPU|stats split. (Wide-2col overrides proc_rows
        // above and lays the body out in view(); these widths are unused there.)
        L.right_w = std::clamp((inner - gap_w) * 42 / 100, 34, 56);
        L.left_w  = inner - gap_w - L.right_w;
        const int cpu_inner = (L.narrow ? inner : L.left_w) - 4;
        L.graph_w = std::max(8, cpu_inner - 4);

        // Anchor the proc-table rows from the BOTTOM, not the top. The 1-row
        // Header widget gets squeezed out of the v-stack when height is tight,
        // which would shift a top-anchored table by one row and mis-route every
        // click. The v-stack is top-aligned with NO trailing blank: the footer
        // paints on the LAST terminal row and the proc panel's bottom border
        // sits directly above it. Counting up from there is exact at any
        // height. (Verified against a live frame dump — footer row 44 of 45.)
        L.footer_y    = m.height - 1;
        const int proc_bot_border = L.footer_y - 1;     // panel bottom border
        const int proc_body_last  = proc_bot_border - 1;// last visible row
        L.proc_body_y = proc_body_last - (L.body_rows - 1);
        L.proc_hdr_y  = L.proc_body_y - 1;              // column-header row
        L.proc_top_y  = L.proc_hdr_y - 1;               // panel top border
        L.top_y       = 4;   // nominal; not used for hit-testing
        return L;
    }

    // ── footer action dispatch ─────────────────────────────────────────
    // Clicks resolve to a ui::FooterAct via the paint-time hit registry
    // (see hit_ids.hpp + Footer's hit() tags); this maps the action to a
    // model transition. No coordinate math — the id came from the same
    // layout pass that painted the hint.
    static std::pair<Model, maya::Cmd<Msg>> dispatch_footer(Model m, ui::FooterAct a) {
        using C = maya::Cmd<Msg>;
        using ui::FooterAct;
        switch (a) {
            case FooterAct::Quit:  return {std::move(m), C::quit()};
            case FooterAct::Filter: m.filtering = true; m.filter.clear(); m.sel = 0; m.scroll_top = 0; return {std::move(m), C{}};
            case FooterAct::End:   return arm_kill(std::move(m), SIGTERM);
            case FooterAct::Kill:  return arm_kill(std::move(m), SIGKILL);
            case FooterAct::Sort:  m.sort = static_cast<SortKey>((static_cast<int>(m.sort) + 1) % 6); return resample(std::move(m));
            case FooterAct::Pause: m.paused = !m.paused; return {std::move(m), C{}};
            case FooterAct::Help:  m.show_help = true; return {std::move(m), C{}};
        }
        return {std::move(m), C{}};
    }

    // ── mouse handling ───────────────────────────────────────────────────────
    static std::pair<Model, maya::Cmd<Msg>> on_mouse(Model m, const maya::MouseEvent& me) {
        using C = maya::Cmd<Msg>;
        using maya::MouseButton;
        using maya::MouseEventKind;

        // maya delivers 1-based cell coords; convert to 0-based frame cells,
        // then resolve the click to a hit-tagged widget by the SAME rect the
        // renderer painted last frame — no hand-mirrored layout math.
        const auto hit = maya::hit_test(me.x.value - 1, me.y.value - 1);
        const int n = static_cast<int>(filtered(m).size());

        // ── Scroll wheel ──
        // In a scrollable detail pane the wheel moves the pane window; over the
        // process table it moves the selection; anywhere else it still scrolls
        // the list so the wheel is never a dead input.
        if (me.button == MouseButton::ScrollDown) {
            if (m.show_help) {
                m.help_scroll += 3; clamp_help_scroll(m); return {std::move(m), C{}};
            }
            if (m.detail != ui::Detail::None && m.detail != ui::Detail::Proc) {
                m.detail_scroll += 3; clamp_detail_scroll(m); return {std::move(m), C{}};
            }
            m.sel += 3; clamp_sel(m);
            if (m.detail == ui::Detail::Proc) pin_detail_pid(m);
            return {std::move(m), C{}};
        }
        if (me.button == MouseButton::ScrollUp) {
            if (m.show_help) {
                m.help_scroll -= 3; clamp_help_scroll(m); return {std::move(m), C{}};
            }
            if (m.detail != ui::Detail::None && m.detail != ui::Detail::Proc) {
                m.detail_scroll -= 3; clamp_detail_scroll(m); return {std::move(m), C{}};
            }
            m.sel -= 3; clamp_sel(m);
            if (m.detail == ui::Detail::Proc) pin_detail_pid(m);
            return {std::move(m), C{}};
        }

        // Only act on button presses for the rest (ignore Move/Release so we
        // don't double-fire; drags fall through harmlessly).
        if (me.kind != MouseEventKind::Press) return {std::move(m), C{}};

        // Modal layers first — a click outside the modal dismisses it.
        if (m.show_help) { m.show_help = false; m.help_scroll = 0; return {std::move(m), C{}}; }
        if (m.detail != ui::Detail::None) {
            // A click on a detail tab switches domain; anywhere else closes.
            if (me.button == MouseButton::Left && hit
                && maya::hit_kind(*hit) == ui::HK_DetailTab) {
                m.detail = static_cast<ui::Detail>(maya::hit_index(*hit));
                m.detail_scroll = 0;
                if (m.detail == ui::Detail::Proc) pin_detail_pid(m);
                return {std::move(m), C{}};
            }
            m.detail = ui::Detail::None;
            m.detail_pid = 0;
            return {std::move(m), C{}};
        }
        if (m.pending) {
            // Footer shows y·confirm / n·cancel; a click anywhere just cancels
            // for safety — killing is keyboard-confirmed only, never a click.
            m.pending.reset();
            return {std::move(m), C{}};
        }
        if (m.sigmenu) {
            // Same safety rule for the signal picker: a click dismisses it;
            // the destructive choice stays keyboard-only.
            m.sigmenu.reset();
            return {std::move(m), C{}};
        }
        if (m.nicemenu) { m.nicemenu.reset(); return {std::move(m), C{}}; }

        // ── Everything else routes through the paint-time hit registry ──
        if (hit) {
            switch (maya::hit_kind(*hit)) {
                case ui::HK_FooterAct:
                    if (me.button == MouseButton::Left)
                        return dispatch_footer(std::move(m),
                            static_cast<ui::FooterAct>(maya::hit_index(*hit)));
                    return {std::move(m), C{}};

                case ui::HK_SortCol:
                    if (me.button == MouseButton::Left && !m.filtering) {
                        m.sort = static_cast<SortKey>(maya::hit_index(*hit));
                        return resample(std::move(m));
                    }
                    return {std::move(m), C{}};

                case ui::HK_BandPanel:
                    if (me.button == MouseButton::Left) {
                        m.detail = static_cast<ui::Detail>(maya::hit_index(*hit));
                        m.detail_scroll = 0;
                        if (m.detail == ui::Detail::Proc) pin_detail_pid(m);
                        return {std::move(m), C{}};
                    }
                    return {std::move(m), C{}};

                case ui::HK_ProcRow: {
                    const int idx = static_cast<int>(maya::hit_index(*hit));
                    if (idx >= 0 && idx < n) {
                        m.sel = idx;
                        if (me.button == MouseButton::Right)
                            return arm_kill(std::move(m), SIGTERM);
                    }
                    return {std::move(m), C{}};
                }

                default:
                    break;
            }
        }

        return {std::move(m), C{}};
    }

    // ── init / update ───────────────────────────────────────────────────────

    static std::pair<Model, maya::Cmd<Msg>> init() {
        Model m;
        // Seed the view from the boot config (CLI flags over the saved file):
        // the sort column + direction, tree/flat, refresh cadence, and an
        // optional startup filter all come back the way you left them.
        const Config& cfg = boot_config();
        m.sort       = cfg.sort;
        m.sort_desc  = cfg.sort_desc;
        m.tree       = cfg.tree;
        m.refresh_ms = cfg.refresh_ms;
        m.filter     = cfg.filter;
        // The very first sample runs synchronously: there is no event loop yet
        // to block, and the first frame should paint with real data instead of
        // an empty snapshot. Every sample AFTER this is a background effect.
        //
        // SNAPPY: CPU% and per-process CPU% are DELTAS across two readings, so a
        // single first sample would paint a full second of zeros. We prime the
        // deltas with a throwaway read + a short (~90ms) settle + the real read,
        // so the very first painted frame already shows true CPU load. ~90ms is
        // imperceptible at launch but turns "1s of zeros" into "instant data".
        m.sampler->sample(m.sort, kTopN);                 // prime delta baselines
        std::this_thread::sleep_for(std::chrono::milliseconds(90));
        m.snap = m.sampler->sample(m.sort, kTopN);        // real, populated frame
        // rockbottom opens on the flat sorted list — the fastest thing to read
        // at a glance; press t for the tree (which opens fully expanded).
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Write the current view state back to the config file on a clean exit, so
    // the tool reopens the way you left it. Best-effort (never blocks quit).
    static void save_config(const Model& m) {
        Config c;
        c.sort       = m.sort;
        c.sort_desc  = m.sort_desc;
        c.tree       = m.tree;
        c.refresh_ms = m.refresh_ms;
        c.filter     = m.filter;
        c.save();
    }

    // Cap on processes carried from the sampler into the UI per tick. 0 = keep
    // EVERYTHING: the walk touches every pid anyway, the table windows/scrolls,
    // and the flow tree needs full parentage (a truncated list orphans kids and
    // hides processes on a Pid/Name sort). Memory cost of ~all procs is trivial.
    static constexpr int kTopN = 0;

    // Describe (do NOT perform) a background sample. Returns a Cmd the runtime
    // runs on a dedicated detached thread; when it finishes it dispatches a
    // Sampled{} message back through update(). task_isolated (not task) is
    // deliberate: sample_gpu() spawns nvidia-smi and reads /proc, /sys, and a
    // wedged syscall (dead FUSE mount, hung subprocess) must leak one thread
    // rather than starve the shared worker pool. Marks the model in-flight so
    // overlapping Ticks are dropped until the result lands.
    static maya::Cmd<Msg> sample_cmd(Model& m) {
        m.sampling = true;
        auto sampler = m.sampler;   // shared_ptr copy: outlives this update()
        SortKey sort = m.sort;
        return maya::Cmd<Msg>::task_isolated(
            [sampler, sort](std::function<void(Msg)> dispatch) {
                dispatch(Sampled{sampler->sample(sort, kTopN)});
            });
    }

    static std::pair<Model, maya::Cmd<Msg>> update(Model m, Msg msg) {
        using C = maya::Cmd<Msg>;
        return std::visit(maya::overload{
            [&](Tick) -> std::pair<Model, C> {
                if (m.toast && --m.toast->ttl <= 0) m.toast.reset();
                if (m.verdict_pulse > 0) --m.verdict_pulse;
                // Kick a background sample unless paused or one's already running.
                if (!m.paused && !m.sampling) {
                    ++m.ticks;
                    C c = sample_cmd(m);
                    return {std::move(m), std::move(c)};
                }
                return {std::move(m), C{}};
            },
            [&](Sampled sm) -> std::pair<Model, C> {
                // Pure fold: the effect already did the I/O off-thread.
                const Health prev = m.last_health;
                m.snap = std::move(sm.snap);
                ++m.snap_gen;   // new data → visual_hash advances → frame renders
                m.last_health = m.snap.verdict.level;
                if (static_cast<int>(m.last_health) > static_cast<int>(prev))
                    m.verdict_pulse = 3;   // degrade → flare for 3 ticks
                m.sampling = false;
                // Follow mode: keep the cursor pinned to the locked process as
                // the freshly-sampled list re-orders around it.
                if (m.follow_pid) select_pid(m, m.follow_pid);
                clamp_sel(m);
                return {std::move(m), C{}};
            },
            [&](Resize r) { m.width = r.w; m.height = r.h; return std::pair{std::move(m), C{}}; },
            [&](Key k)    { return on_key(std::move(m), k.ev); },
            [&](Mouse mo) { return on_mouse(std::move(m), mo.ev); },
            [&](Quit)     { save_config(m); return std::pair{std::move(m), C::quit()}; },
        }, msg);
    }

    // ── key handling: one place, mode-aware ─────────────────────────────────

    static std::pair<Model, maya::Cmd<Msg>> on_key(Model m, const maya::KeyEvent& ke) {
        using C = maya::Cmd<Msg>;
        maya::Event ev{ke};
        using maya::key;

        // 0. Signal picker intercepts everything. Number keys 1-9 jump to a
        //    signal; ↑↓ move; enter/y arms the confirm; esc/n backs out.
        if (m.sigmenu) {
            const auto& cat = signal_catalog();
            const int n = static_cast<int>(cat.size());
            if (key(ev, maya::SpecialKey::Escape) || key(ev, 'n') || key(ev, 'q')) {
                m.sigmenu.reset();
                return {std::move(m), C{}};
            }
            if (key(ev, maya::SpecialKey::Down) || key(ev, 'j')) {
                m.sigmenu->sel = std::min(n - 1, m.sigmenu->sel + 1); return {std::move(m), C{}};
            }
            if (key(ev, maya::SpecialKey::Up) || key(ev, 'k')) {
                m.sigmenu->sel = std::max(0, m.sigmenu->sel - 1); return {std::move(m), C{}};
            }
            if (auto* ck = std::get_if<maya::CharKey>(&ke.key);
                ck && ck->codepoint >= '1' && ck->codepoint <= '9') {
                const int idx = static_cast<int>(ck->codepoint - '1');
                if (idx < n) m.sigmenu->sel = idx;
                return {std::move(m), C{}};
            }
            if (key(ev, maya::SpecialKey::Enter) || key(ev, 'y')) {
                const int sig = cat[static_cast<std::size_t>(
                    std::clamp(m.sigmenu->sel, 0, n - 1))].num;
                m.pending = PendingKill{m.sigmenu->anchor_pid, m.sigmenu->name,
                                        sig, m.sigmenu->pids};
                m.sigmenu.reset();
                return {std::move(m), C{}};
            }
            return {std::move(m), C{}};
        }

        // 0b. Renice dial. ←→/↑↓ adjust; enter applies via setpriority(2).
        if (m.nicemenu) {
            if (key(ev, maya::SpecialKey::Escape) || key(ev, 'q')) {
                m.nicemenu.reset();
                return {std::move(m), C{}};
            }
            if (key(ev, maya::SpecialKey::Left) || key(ev, maya::SpecialKey::Down)
                || key(ev, 'h') || key(ev, 'j')) {
                m.nicemenu->val = std::max(-20, m.nicemenu->val - 1); return {std::move(m), C{}};
            }
            if (key(ev, maya::SpecialKey::Right) || key(ev, maya::SpecialKey::Up)
                || key(ev, 'l') || key(ev, 'k')) {
                m.nicemenu->val = std::min(19, m.nicemenu->val + 1); return {std::move(m), C{}};
            }
            if (key(ev, maya::SpecialKey::PageDown)) { m.nicemenu->val = std::max(-20, m.nicemenu->val - 5); return {std::move(m), C{}}; }
            if (key(ev, maya::SpecialKey::PageUp))   { m.nicemenu->val = std::min(19, m.nicemenu->val + 5); return {std::move(m), C{}}; }
            if (key(ev, maya::SpecialKey::Enter) || key(ev, 'y')) {
                std::string err = renice_process(m.nicemenu->pid, m.nicemenu->val);
                if (err.empty())
                    m.toast = Toast{"reniced " + m.nicemenu->name + " to " +
                                    (m.nicemenu->val > 0 ? "+" : "") + std::to_string(m.nicemenu->val), false};
                else
                    m.toast = Toast{err, true};
                m.nicemenu.reset();
                if (!m.sampling) { auto c = sample_cmd(m); return {std::move(m), std::move(c)}; }
                return {std::move(m), C{}};
            }
            return {std::move(m), C{}};
        }

        // 1. Kill confirmation intercepts everything.
        if (m.pending) {
            if (key(ev, 'y') || key(ev, maya::SpecialKey::Enter)) {
                const auto& targets = m.pending->pids;
                int ok = 0, failed = 0;
                std::string first_err;
                for (int pid : targets) {
                    std::string err = signal_process(pid, m.pending->sig);
                    if (err.empty()) ++ok;
                    else { ++failed; if (first_err.empty()) first_err = err; }
                }
                const bool group = targets.size() > 1;
                const int sig = m.pending->sig;
                // Signal-aware wording: KILL is "force-killed", STOP/CONT/HUP
                // etc. name themselves, plain TERM stays "asked … to exit".
                std::string verb, tail;
                if (sig == SIGKILL)      { verb = "force-killed "; tail = ""; }
                else if (sig == SIGTERM) { verb = "asked ";        tail = " to exit"; }
                else if (sig == SIGSTOP || sig == SIGTSTP) { verb = "suspended "; tail = ""; }
                else if (sig == SIGCONT) { verb = "resumed ";      tail = ""; }
                else { verb = "sent " + sig_name(sig) + " to "; tail = ""; }
                std::string what = group
                    ? std::to_string(ok) + " × " + m.pending->name
                    : m.pending->name + " (" + std::to_string(m.pending->pid) + ")";
                if (failed)
                    m.toast = Toast{first_err + (group ? " (+" + std::to_string(failed - 1) + " more failed)"
                                                        : ""), true};
                else
                    m.toast = Toast{verb + what + tail, false};
                m.pending.reset();
                // Refresh the list off-thread so killed processes drop out
                // promptly without blocking on a full re-sample here.
                if (!m.sampling) { auto c = sample_cmd(m); return {std::move(m), std::move(c)}; }
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
            m.scroll_top = 0;
            return {std::move(m), C{}};
        }

        // 3. Help overlay: scrollable; the usual dismiss keys close it.
        if (m.show_help) {
            if (key(ev, '?') || key(ev, 'h') || key(ev, maya::SpecialKey::Escape) || key(ev, 'q')) {
                m.show_help = false; m.help_scroll = 0;
            }
            else if (key(ev, maya::SpecialKey::Down) || key(ev, 'j')) { m.help_scroll += 1; clamp_help_scroll(m); }
            else if (key(ev, maya::SpecialKey::Up)   || key(ev, 'k')) { m.help_scroll -= 1; clamp_help_scroll(m); }
            else if (key(ev, maya::SpecialKey::PageDown) || key(ev, ' ')) { m.help_scroll += 10; clamp_help_scroll(m); }
            else if (key(ev, maya::SpecialKey::PageUp))  { m.help_scroll -= 10; clamp_help_scroll(m); }
            else if (key(ev, maya::SpecialKey::Home) || key(ev, 'g')) { m.help_scroll = 0; }
            else if (key(ev, maya::SpecialKey::End)  || key(ev, 'G')) { m.help_scroll = 1 << 20; clamp_help_scroll(m); }
            return {std::move(m), C{}};
        }

        // 3b. Detail pane (full-screen drill-down): Esc/q close it, the
        // number keys switch domain, and in the process view x/K still work.
        if (m.detail != ui::Detail::None) {
            if (key(ev, maya::SpecialKey::Escape) || key(ev, 'q')) {
                m.detail = ui::Detail::None; m.detail_scroll = 0; m.detail_pid = 0;
                return {std::move(m), C{}};
            }
            if (key(ev, '1')) { m.detail = ui::Detail::Cpu;  m.detail_scroll = 0; return {std::move(m), C{}}; }
            if (key(ev, '2')) { m.detail = ui::Detail::Mem;  m.detail_scroll = 0; return {std::move(m), C{}}; }
            if (key(ev, '3')) { m.detail = ui::Detail::Net;  m.detail_scroll = 0; return {std::move(m), C{}}; }
            if (key(ev, '4')) { m.detail = ui::Detail::Gpu;  m.detail_scroll = 0; return {std::move(m), C{}}; }
            if (key(ev, '5')) { m.detail = ui::Detail::Disk; m.detail_scroll = 0; return {std::move(m), C{}}; }
            if (key(ev, '6')) { m.detail = ui::Detail::Proc; m.detail_scroll = 0; pin_detail_pid(m); return {std::move(m), C{}}; }
            if (m.detail == ui::Detail::Proc) {
                // ↑↓ walk the table selection AND re-pin the pane to the new
                // row, so the pane follows deliberate navigation but never
                // drifts on its own when the table resorts under it.
                if (key(ev, maya::SpecialKey::Down) || key(ev, 'j')) { ++m.sel; clamp_sel(m); pin_detail_pid(m); return {std::move(m), C{}}; }
                if (key(ev, maya::SpecialKey::Up)   || key(ev, 'k')) { --m.sel; clamp_sel(m); pin_detail_pid(m); return {std::move(m), C{}}; }
                // ←/→ walk the FAMILY: up to the parent, down into the busiest
                // child — turning the detail pane into a tree explorer you never
                // have to leave. Re-pins pane + table selection together.
                if (key(ev, maya::SpecialKey::Left)  || key(ev, 'h')) return nav_family(std::move(m), /*to_parent=*/true);
                if (key(ev, maya::SpecialKey::Right) || key(ev, 'l')) return nav_family(std::move(m), /*to_parent=*/false);
                if (key(ev, maya::SpecialKey::Enter)) {
                    m.detail = ui::Detail::None; m.detail_scroll = 0; m.detail_pid = 0;
                    return {std::move(m), C{}};
                }
                if (key(ev, 'x') || key(ev, maya::SpecialKey::Delete)) return arm_kill(std::move(m), SIGTERM);
                if (key(ev, 'K')) return arm_kill(std::move(m), SIGKILL);
                if (key(ev, 'X')) return arm_kill_all(std::move(m), SIGTERM);
                if (key(ev, 'T')) return arm_kill_subtree(std::move(m), SIGTERM);
                if (key(ev, 'l')) return open_sigmenu(std::move(m));
                if (key(ev, 'r')) return open_nicemenu(std::move(m));
                if (key(ev, maya::SpecialKey::PageDown)) { m.detail_scroll += 10; clamp_detail_scroll(m); return {std::move(m), C{}}; }
                if (key(ev, maya::SpecialKey::PageUp))   { m.detail_scroll -= 10; clamp_detail_scroll(m); return {std::move(m), C{}}; }
            } else {
                if (key(ev, maya::SpecialKey::Enter)) {
                    m.detail = ui::Detail::None; m.detail_scroll = 0;
                    return {std::move(m), C{}};
                }
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
            if (!m.filter.empty()) { m.filter.clear(); m.sel = 0; m.scroll_top = 0; return {std::move(m), C{}}; }
            save_config(m);
            return {std::move(m), C::quit()};
        }
        if (key(ev, 'p') || key(ev, ' '))  { m.paused = !m.paused; return {std::move(m), C{}}; }
        // Refresh cadence: < slower, > faster, clamped 250ms–5s. A toast
        // confirms the new rate; the subscription re-times on next update.
        if (key(ev, '>') || key(ev, '.')) {
            m.refresh_ms = std::max(250, m.refresh_ms - 250);
            m.toast = Toast{"refresh " + refresh_label(m.refresh_ms), false};
            return {std::move(m), C{}};
        }
        if (key(ev, '<') || key(ev, ',')) {
            m.refresh_ms = std::min(5000, m.refresh_ms + 250);
            m.toast = Toast{"refresh " + refresh_label(m.refresh_ms), false};
            return {std::move(m), C{}};
        }
        if (key(ev, '?') || key(ev, 'h'))  { m.show_help = true; return {std::move(m), C{}}; }
        if (key(ev, '/'))                  { m.filtering = true; m.filter.clear(); m.sel = 0; m.scroll_top = 0; return {std::move(m), C{}}; }

        // Detail drill-down: 1-5 open a full-screen domain view; Enter opens
        // the selected process's detail.
        if (key(ev, '1')) { m.detail = ui::Detail::Cpu;  m.detail_scroll = 0; return {std::move(m), C{}}; }
        if (key(ev, '2')) { m.detail = ui::Detail::Mem;  m.detail_scroll = 0; return {std::move(m), C{}}; }
        if (key(ev, '3')) { m.detail = ui::Detail::Net;  m.detail_scroll = 0; return {std::move(m), C{}}; }
        if (key(ev, '4')) { m.detail = ui::Detail::Gpu;  m.detail_scroll = 0; return {std::move(m), C{}}; }
        if (key(ev, '5')) { m.detail = ui::Detail::Disk; m.detail_scroll = 0; return {std::move(m), C{}}; }
        if (key(ev, '6') || key(ev, maya::SpecialKey::Enter)) {
            m.detail = ui::Detail::Proc; m.detail_scroll = 0; pin_detail_pid(m);
            return {std::move(m), C{}};
        }

        if (key(ev, 's')) { m.sort = static_cast<SortKey>((static_cast<int>(m.sort) + 1) % 6); m.sort_desc = true; return resample(std::move(m)); }
        if (key(ev, 'c')) return set_sort(std::move(m), SortKey::Cpu);
        if (key(ev, 'm')) return set_sort(std::move(m), SortKey::Mem);
        if (key(ev, 'i')) return set_sort(std::move(m), SortKey::Io);
        if (key(ev, 'P')) return set_sort(std::move(m), SortKey::Pid);
        if (key(ev, 'n')) return set_sort(std::move(m), SortKey::Name);
        if (key(ev, 'o')) return set_sort(std::move(m), SortKey::Port);
        if (key(ev, 'R')) { m.sort_desc = !m.sort_desc; const int keep = selected_pid(m); select_pid(m, keep); return {std::move(m), C{}}; }

        // Tree view + navigation.
        if (key(ev, 't')) return toggle_tree(std::move(m));
        if (key(ev, '*')) return toggle_follow(std::move(m));
        if (m.tree && (key(ev, maya::SpecialKey::Left)  || key(ev, 'h'))) return set_collapse(std::move(m), true);
        if (m.tree && (key(ev, maya::SpecialKey::Right) || key(ev, 'l'))) return set_collapse(std::move(m), false);
        if (m.tree && key(ev, '=')) return collapse_all(std::move(m));
        if (m.tree && key(ev, '+')) return expand_all(std::move(m));

        // Selection.
        if (key(ev, maya::SpecialKey::Down) || key(ev, 'j')) { ++m.sel; clamp_sel(m); m.follow_pid = 0; return {std::move(m), C{}}; }
        if (key(ev, maya::SpecialKey::Up)   || key(ev, 'k')) { --m.sel; clamp_sel(m); m.follow_pid = 0; return {std::move(m), C{}}; }
        if (key(ev, maya::SpecialKey::Home)) { m.sel = 0; m.scroll_top = 0; m.follow_pid = 0; return {std::move(m), C{}}; }
        if (key(ev, maya::SpecialKey::End))  { m.sel = 1 << 20; clamp_sel(m); m.follow_pid = 0; return {std::move(m), C{}}; }
        if (key(ev, maya::SpecialKey::PageDown)) { m.sel += 10; clamp_sel(m); m.follow_pid = 0; return {std::move(m), C{}}; }
        if (key(ev, maya::SpecialKey::PageUp))   { m.sel -= 10; clamp_sel(m); m.follow_pid = 0; return {std::move(m), C{}}; }

        // Kill.
        if (key(ev, 'x') || key(ev, maya::SpecialKey::Delete)) return arm_kill(std::move(m), SIGTERM);
        if (key(ev, 'K'))                                      return arm_kill(std::move(m), SIGKILL);
        if (key(ev, 'X'))                                      return arm_kill_all(std::move(m), SIGTERM);
        if (key(ev, 'l') && !m.tree)                           return open_sigmenu(std::move(m));
        if (key(ev, 'r'))                                      return open_nicemenu(std::move(m));

        return {std::move(m), C{}};
    }

    // ── helpers ─────────────────────────────────────────────────────────────

    // Hash the inputs order_procs() depends on. Collision would return a stale
    // view for one frame at worst; the space is astronomically large.
    static std::uint64_t ordered_key(const Model& m) {
        std::uint64_t h = 1469598103934665603ULL;
        auto fold = [&](std::uint64_t v) { h ^= v; h *= 1099511628211ULL; };
        fold(m.snap_gen);
        fold(static_cast<std::uint64_t>(m.sort));
        fold(m.sort_desc ? 1 : 0);
        fold(m.tree ? 3 : 2);
        for (unsigned char c : m.filter) fold(c);
        fold(m.filter.size());
        if (m.tree) for (int pid : m.collapsed) fold(static_cast<std::uint64_t>(pid) * 2654435761u);
        return h;
    }

    // The fully-ordered process view (filter → sort/tree). MEMOIZED: recomputes
    // only when the input hash changes, so a whole frame's worth of
    // ordered()/filtered()/selected_pid() calls costs one order_procs() at most.
    // Returns a const reference into the cache — callers must not outlive it,
    // which they never do (all uses are within one update()/view()).
    static const ui::OrderedProcs& ordered(const Model& m) {
        const std::uint64_t key = ordered_key(m);
        auto& c = *m.ord_cache;
        if (c.key != key) {
            c.value = ui::order_procs(m.snap.procs, m.filter, m.sort, m.sort_desc,
                                      m.tree, m.collapsed);
            c.key = key;
        }
        return c.value;
    }

    static const std::vector<const ProcInfo*>& filtered(const Model& m) {
        return ordered(m).procs;
    }

    static void clamp_sel(Model& m) {
        int n = static_cast<int>(filtered(m).size());
        m.sel = std::clamp(m.sel, 0, std::max(0, n - 1));
        sync_scroll(m);
    }

    // Sticky scroll window (vim/htop "scrolloff"). The window top is a
    // PERSISTED value (m.scroll_top): it only moves when the cursor would come
    // within `margin` rows of the top or bottom edge, so walking through the
    // middle of the list leaves the viewport rock-still and the surrounding
    // rows stay as context — instead of the cursor being glued to the last row
    // while everything scrolls under it. When the whole list fits, top pins to
    // 0; a short tail can't scroll past the last full screen.
    static void sync_scroll(Model& m) {
        const int n = static_cast<int>(filtered(m).size());
        const int body_rows = compute_layout(m).body_rows;
        if (n <= body_rows) { m.scroll_top = 0; return; }
        // Keep this many rows of context between the cursor and the edge (but
        // never more than fits either side of centre).
        const int margin = std::clamp(body_rows / 4, 1, 4);
        int top = std::clamp(m.scroll_top, 0, std::max(0, n - body_rows));
        if (m.sel < top + margin)                 top = m.sel - margin;          // scroll up
        else if (m.sel > top + body_rows - 1 - margin) top = m.sel - body_rows + 1 + margin; // scroll down
        m.scroll_top = std::clamp(top, 0, std::max(0, n - body_rows));
    }

    // The pid under the cursor in the current ordered view, or 0 if none.
    static int selected_pid(const Model& m) {
        const auto& view = filtered(m);
        if (!view.empty() && m.sel >= 0 && m.sel < static_cast<int>(view.size()))
            return view[static_cast<std::size_t>(m.sel)]->pid;
        return 0;
    }

    // Re-point the selection at a given pid after the list re-orders (toggling
    // tree, sort, collapse). Keeps the cursor on the SAME process instead of
    // the same index, which is the difference between "solid" and "jumpy".
    static void select_pid(Model& m, int pid) {
        if (pid <= 0) return;
        const auto& view = filtered(m);
        for (std::size_t i = 0; i < view.size(); ++i)
            if (view[i]->pid == pid) { m.sel = static_cast<int>(i); sync_scroll(m); return; }
        clamp_sel(m);
    }

    // Toggle the process-tree view, keeping the cursor on the same process.
    static std::pair<Model, maya::Cmd<Msg>> toggle_tree(Model m) {
        const int keep = selected_pid(m);
        m.tree = !m.tree;
        // Tree opens FULLY EXPANDED (collapsed set stays empty); leaving tree
        // clears any folds so re-entry is a clean, fully-open slate.
        if (!m.tree) m.collapsed.clear();
        select_pid(m, keep);
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Collapse (fold=true) or expand (fold=false) the selected subtree. In
    // flat mode this is a no-op. Collapsing a leaf jumps to its parent so
    // ←/→ feels like real tree navigation (htop idiom).
    static std::pair<Model, maya::Cmd<Msg>> set_collapse(Model m, bool fold) {
        if (!m.tree) return {std::move(m), maya::Cmd<Msg>{}};
        // Snapshot the row's facts out of the (cached) view BEFORE any mutation,
        // because select_pid() below recomputes the cache and would invalidate
        // a held reference.
        int pid = 0, ppid = 0; bool kids = false;
        {
            const ui::OrderedProcs& ord = ordered(m);
            if (m.sel < 0 || m.sel >= static_cast<int>(ord.procs.size()))
                return {std::move(m), maya::Cmd<Msg>{}};
            const auto* p = ord.procs[static_cast<std::size_t>(m.sel)];
            pid = p->pid; ppid = p->ppid;
            kids = m.sel < static_cast<int>(ord.has_kids.size())
                   && ord.has_kids[static_cast<std::size_t>(m.sel)];
        }
        if (fold) {
            if (kids && !m.collapsed.count(pid)) { m.collapsed.insert(pid); }
            else {
                // Already a leaf or already folded → hop to the parent so a
                // repeated ← walks up the tree.
                select_pid(m, ppid);
            }
        } else {
            if (kids && m.collapsed.count(pid)) m.collapsed.erase(pid);
        }
        select_pid(m, pid);
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Toggle collapse on the selected row (Space / Enter-on-parent).
    static std::pair<Model, maya::Cmd<Msg>> toggle_collapse(Model m) {
        if (!m.tree) return {std::move(m), maya::Cmd<Msg>{}};
        const int pid = selected_pid(m);
        if (pid <= 0) return {std::move(m), maya::Cmd<Msg>{}};
        if (m.collapsed.count(pid)) m.collapsed.erase(pid);
        else                        m.collapsed.insert(pid);
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Fold EVERY subtree to its roots (btop/broot "fit to screen"): collapse
    // every process that has children in the current view. The cursor stays on
    // its process — if that row got folded away under an ancestor, select_pid
    // resolves to the nearest visible row.
    static std::pair<Model, maya::Cmd<Msg>> collapse_all(Model m) {
        if (!m.tree) return {std::move(m), maya::Cmd<Msg>{}};
        const int keep = selected_pid(m);
        // Build the parent set from the UNFILTERED, UNCOLLAPSED tree so we fold
        // every real parent, not just the ones currently expanded/visible.
        std::set<int> empty;
        ui::OrderedProcs full = ui::order_procs(m.snap.procs, m.filter, m.sort,
                                                m.sort_desc, true, empty);
        m.collapsed.clear();
        for (std::size_t i = 0; i < full.procs.size(); ++i)
            if (i < full.has_kids.size() && full.has_kids[i])
                m.collapsed.insert(full.procs[i]->pid);
        select_pid(m, keep);
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Expand everything: clear the fold set so the whole tree is open.
    static std::pair<Model, maya::Cmd<Msg>> expand_all(Model m) {
        if (!m.tree) return {std::move(m), maya::Cmd<Msg>{}};
        const int keep = selected_pid(m);
        m.collapsed.clear();
        select_pid(m, keep);
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Lock/unlock "follow": keep the selected process under the cursor as the
    // list re-sorts each tick, instead of the row index drifting.
    static std::pair<Model, maya::Cmd<Msg>> toggle_follow(Model m) {
        const int pid = selected_pid(m);
        m.follow_pid = (m.follow_pid == pid) ? 0 : pid;
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Pick a sort column. Re-pressing the ACTIVE column flips direction
    // (btop/htop idiom); switching columns resets to the natural "biggest
    // first" order.
    static std::pair<Model, maya::Cmd<Msg>> set_sort(Model m, SortKey k) {
        const int keep = selected_pid(m);
        if (m.sort == k) m.sort_desc = !m.sort_desc;
        else            { m.sort = k; m.sort_desc = true; }
        select_pid(m, keep);
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Remember WHICH process the Proc detail pane is showing. The table
    // resorts on every tick (cpu% moves), so an index would silently swap the
    // pane to a different process — the PID pins it to the one you chose.
    static void pin_detail_pid(Model& m) {
        const auto& view = filtered(m);
        m.detail_pid = (!view.empty() && m.sel < static_cast<int>(view.size()))
                           ? view[static_cast<std::size_t>(m.sel)]->pid : 0;
    }

    // Resolve the pinned PID to a live row; nullptr when it exited.
    static const ProcInfo* pinned_proc(const Model& m) {
        for (const auto& p : m.snap.procs)
            if (p.pid == m.detail_pid) return &p;
        return nullptr;
    }

    // Clamp the detail-pane scroll offset to [0, content - viewport]. Builds a
    // throwaway DetailPane to ask it how tall its body is at the current size.
    static void clamp_detail_scroll(Model& m) {
        // The NET pane in wide/split mode scrolls its CONNECTIONS column
        // independently, so its scroll ceiling is the socket count minus the
        // table viewport, not the generic body/viewport delta.
        if (m.detail == ui::Detail::Net) {
            ui::detail::Ctx cx = ui::detail::Ctx::make(m.width, m.height, 0);
            int cmax = ui::detail::net_conn_scroll_max(m.snap, cx);
            if (cmax >= 0) { m.detail_scroll = std::clamp(m.detail_scroll, 0, cmax); return; }
        }
        const ProcInfo* p = m.detail == ui::Detail::Proc ? pinned_proc(m) : nullptr;
        ui::DetailPane pane{m.snap, m.detail, p, m.width, m.height, 0};
        int max_scroll = std::max(0, pane.content_rows() - pane.viewport_rows());
        m.detail_scroll = std::clamp(m.detail_scroll, 0, max_scroll);
    }

    static void clamp_help_scroll(Model& m) {
        const int max_scroll = std::max(0, ui::HelpOverlay::content_rows(m.width)
                                           - ui::HelpOverlay::viewport_rows(m.height));
        m.help_scroll = std::clamp(m.help_scroll, 0, max_scroll);
    }

    // Open the signal picker on the selected process (single target). The
    // menu itself arms the PendingKill once a signal is chosen.
    static std::pair<Model, maya::Cmd<Msg>> open_sigmenu(Model m) {
        const auto& view = filtered(m);
        if (!view.empty() && m.sel < static_cast<int>(view.size())) {
            const ProcInfo* p = view[static_cast<std::size_t>(m.sel)];
            m.sigmenu = Model::SigMenu{p->pid, p->name, {p->pid}, 0};
        }
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Open the renice dial on the selected process, seeded with its current
    // nice value so ←→ nudge from where it already is.
    static std::pair<Model, maya::Cmd<Msg>> open_nicemenu(Model m) {
        const auto& view = filtered(m);
        if (!view.empty() && m.sel < static_cast<int>(view.size())) {
            const ProcInfo* p = view[static_cast<std::size_t>(m.sel)];
            m.nicemenu = Model::NiceMenu{p->pid, p->name, p->nice, p->nice};
        }
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    static std::pair<Model, maya::Cmd<Msg>> arm_kill(Model m, int sig) {
        const auto& view = filtered(m);
        if (!view.empty() && m.sel < static_cast<int>(view.size())) {
            const ProcInfo* p = view[static_cast<std::size_t>(m.sel)];
            m.pending = PendingKill{p->pid, p->name, sig, {p->pid}};
        }
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Arm a kill for EVERY process sharing the selected row's name (the
    // "kill all Chrome Helpers" move). Same keyboard-confirm flow — the
    // confirm strip shows the count so there are no surprises.
    static std::pair<Model, maya::Cmd<Msg>> arm_kill_all(Model m, int sig) {
        const auto& view = filtered(m);
        if (!view.empty() && m.sel < static_cast<int>(view.size())) {
            const ProcInfo* p = view[static_cast<std::size_t>(m.sel)];
            std::vector<int> pids;
            for (const auto& q : m.snap.procs)
                if (q.name == p->name) pids.push_back(q.pid);
            if (pids.empty()) pids.push_back(p->pid);
            m.pending = PendingKill{p->pid, p->name, sig, std::move(pids)};
        }
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Arm a kill of the WHOLE subtree under the pinned process (this + every
    // descendant), pid-collected by walking the parent map. The "reap this
    // process group" move — one confirm, the strip shows the count. Targets
    // m.detail_pid so it works from the pane regardless of table selection.
    static std::pair<Model, maya::Cmd<Msg>> arm_kill_subtree(Model m, int sig) {
        const int root = m.detail == ui::Detail::Proc ? m.detail_pid : selected_pid(m);
        if (root <= 0) return {std::move(m), maya::Cmd<Msg>{}};
        const ProcInfo* rp = nullptr;
        std::unordered_map<int, std::vector<int>> kids_of;
        for (const auto& q : m.snap.procs) {
            if (q.pid == root) rp = &q;
            if (q.ppid != q.pid) kids_of[q.ppid].push_back(q.pid);
        }
        std::vector<int> pids{root};
        {
            std::vector<int> stk{root};
            while (!stk.empty()) {
                int cur = stk.back(); stk.pop_back();
                if (auto it = kids_of.find(cur); it != kids_of.end())
                    for (int c : it->second) { pids.push_back(c); stk.push_back(c); }
            }
        }
        std::string name = rp ? rp->name : ("pid " + std::to_string(root));
        // The confirm strip's group-wording keys off pids.size()>1, so a
        // subtree of one (a leaf) still reads as a single-target kill.
        m.pending = PendingKill{root, name + " +subtree", sig, std::move(pids)};
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Walk the FAMILY from the detail pane: to_parent hops up to the ppid, else
    // down into the busiest child. Re-pins m.detail_pid and best-effort syncs
    // m.sel so leaving the pane lands the cursor on the same process.
    static std::pair<Model, maya::Cmd<Msg>> nav_family(Model m, bool to_parent) {
        const int cur = m.detail_pid;
        if (cur <= 0) return {std::move(m), maya::Cmd<Msg>{}};
        const ProcInfo* self = nullptr;
        for (const auto& q : m.snap.procs) if (q.pid == cur) { self = &q; break; }
        if (!self) return {std::move(m), maya::Cmd<Msg>{}};

        int target = 0;
        if (to_parent) {
            if (self->ppid > 0 && self->ppid != self->pid)
                for (const auto& q : m.snap.procs)
                    if (q.pid == self->ppid) { target = q.pid; break; }
        } else {
            // Busiest direct child by cpu.
            double best = -1;
            for (const auto& q : m.snap.procs)
                if (q.ppid == cur && q.pid != cur && q.cpu > best) { best = q.cpu; target = q.pid; }
        }
        if (target <= 0) return {std::move(m), maya::Cmd<Msg>{}};
        m.detail_pid = target;
        m.detail_scroll = 0;
        // If the target is visible in the current ordered list, move the cursor
        // there too; if it's folded away, at least the pane follows.
        const auto& view = filtered(m);
        for (std::size_t i = 0; i < view.size(); ++i)
            if (view[i]->pid == target) { m.sel = static_cast<int>(i); break; }
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Human refresh label: "1.0s" / "250ms" for the toast + chip.
    static std::string refresh_label(int ms) {
        if (ms % 1000 == 0) return std::to_string(ms / 1000) + "s";
        if (ms >= 1000) {
            char b[16]; std::snprintf(b, sizeof b, "%.2gs", ms / 1000.0); return b;
        }
        return std::to_string(ms) + "ms";
    }

    static std::pair<Model, maya::Cmd<Msg>> resample(Model m) {
        // Sort changed: re-sample in the background rather than blocking the
        // keystroke. The list keeps showing the old order for one frame, then
        // Sampled{} folds in the re-sorted snapshot. If a sample is already in
        // flight we let it finish (its result will already carry the new sort).
        clamp_sel(m);
        if (m.sampling) return {std::move(m), maya::Cmd<Msg>{}};
        auto c = sample_cmd(m);
        return {std::move(m), std::move(c)};
    }

    // ── subscriptions ───────────────────────────────────────────────────────

    // ── visual_hash: skip view()+render() when nothing visible changed ──────
    // maya's run loop hashes the model before each render and skips the whole
    // view()+layout+paint+diff pipeline when the hash is unchanged. We fold
    // EVERY field the frame's appearance depends on; anything omitted would
    // freeze a real visual change, anything spurious would defeat the skip.
    //
    // Payoff: a PAUSED monitor renders zero frames between keystrokes (the
    // spinner is a static badge, the snapshot is frozen), and even a running
    // monitor skips the Tick frames that merely KICK a sample — only the
    // Sampled fold (snap_gen++) actually repaints. Idle CPU drops to ~0.
    static std::uint64_t visual_hash(const Model& m) {
        std::uint64_t h = 1469598103934665603ULL;
        auto fold = [&](std::uint64_t v) { h ^= v; h *= 1099511628211ULL; };
        auto fold_str = [&](const std::string& s) {
            for (unsigned char c : s) fold(c);
            fold(s.size());
        };

        // Data + geometry: a new sample (snap_gen) or resize repaints all.
        fold(m.snap_gen);
        fold(static_cast<std::uint64_t>(m.width));
        fold(static_cast<std::uint64_t>(m.height));

        // Interaction / mode state that view() branches on.
        fold(static_cast<std::uint64_t>(m.sort));
        fold(m.sort_desc ? 1 : 0);
        fold(m.tree ? 1 : 0);
        fold(static_cast<std::uint64_t>(m.follow_pid));
        for (int pid : m.collapsed) fold(static_cast<std::uint64_t>(pid));
        fold(static_cast<std::uint64_t>(m.sel));
        fold(static_cast<std::uint64_t>(m.scroll_top));
        fold(m.paused ? 1 : 0);
        fold(static_cast<std::uint64_t>(m.refresh_ms));
        fold(m.filtering ? 1 : 0);
        fold_str(m.filter);
        fold(static_cast<std::uint64_t>(m.detail));
        fold(static_cast<std::uint64_t>(m.detail_scroll));
        fold(static_cast<std::uint64_t>(m.detail_pid));
        fold(m.show_help ? 1 : 0);
        fold(static_cast<std::uint64_t>(m.help_scroll));
        fold(static_cast<std::uint64_t>(m.verdict_pulse));

        // Pending kill strip (footer + proc panel both change).
        if (m.pending) { fold(7); fold(static_cast<std::uint64_t>(m.pending->pids.size()));
                         fold(static_cast<std::uint64_t>(m.pending->sig)); fold_str(m.pending->name); }
        // Signal picker overlay: target + which signal is highlighted.
        if (m.sigmenu) { fold(23); fold(static_cast<std::uint64_t>(m.sigmenu->sel));
                         fold(static_cast<std::uint64_t>(m.sigmenu->pids.size()));
                         fold(static_cast<std::uint64_t>(m.sigmenu->anchor_pid)); }
        // Renice dial: which process + the dialed value.
        if (m.nicemenu) { fold(29); fold(static_cast<std::uint64_t>(m.nicemenu->pid));
                          fold(static_cast<std::uint64_t>(m.nicemenu->val + 64)); }
        // Toast: text + error tint (its ttl countdown is what expires it).
        if (m.toast) { fold(m.toast->error ? 11 : 13); fold_str(m.toast->text); }

        // Footer heartbeat spinner advances one frame PER TICK — but only when
        // it's actually shown (not paused, no toast, no pending). Fold ticks
        // only in that case so a paused/notified UI can settle to zero renders.
        if (!m.paused && !m.toast && !m.pending)
            fold(static_cast<std::uint64_t>(m.ticks));

        return h;
    }

    static maya::Sub<Msg> subscribe(const Model& m) {
        using S = maya::Sub<Msg>;
        using namespace std::chrono_literals;
        std::vector<S> subs;
        // Sample cadence is model-driven (< > adjust it); maya re-subscribes
        // after each update so a changed interval takes effect immediately.
        subs.push_back(S::every(std::chrono::milliseconds(std::clamp(m.refresh_ms, 250, 5000)),
                                Tick{}));
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

        if (m.show_help) return HelpOverlay{m.width, m.height, m.help_scroll};

        if (m.sigmenu) {
            const bool group = m.sigmenu->pids.size() > 1;
            std::string target = group
                ? std::to_string(m.sigmenu->pids.size()) + " × " + m.sigmenu->name
                : m.sigmenu->name + " (" + std::to_string(m.sigmenu->anchor_pid) + ")";
            return SignalMenu{m.width, m.height, std::move(target), m.sigmenu->sel};
        }

        if (m.nicemenu) {
            std::string target = m.nicemenu->name + " (" + std::to_string(m.nicemenu->pid) + ")";
            return NiceMenu{m.width, m.height, std::move(target), m.nicemenu->cur, m.nicemenu->val};
        }

        if (m.detail != ui::Detail::None) {
            const ProcInfo* p = m.detail == ui::Detail::Proc ? pinned_proc(m) : nullptr;
            return DetailPane{m.snap, m.detail, p, m.width, m.height, m.detail_scroll,
                              m.pending ? &*m.pending : nullptr};
        }

        const Snapshot& s = m.snap;
        const bool narrow = m.width < 96;

        // ── Height budget ──
        // Fixed rows: header(1) + verdict(3) + footer(1) + outer padding.
        // Top band: CPU panel (graph 4 + blank + cores) vs MEM+NET+DISK stack.
        const int ncores = static_cast<int>(s.cpu.cores.size());
        const bool wide_screen = m.width >= 200;
        // In the wide-2col layout the CPU card lives in a ~narrow left column,
        // so keep the core grid to 2-3 columns (meters need room); only the
        // legacy full-width wide layout packs them 6-8 wide.
        const bool wide2_cores = m.width >= 200;
        const int cpu_cols = wide2_cores ? (ncores > 16 ? 3 : 2)
                           : wide_screen ? (ncores > 32 ? 8 : ncores > 8 ? 6 : 2)
                                         : (ncores > 24 ? 4 : ncores > 12 ? 3 : 2);
        // Narrow mode packs the cores into a single heat-strip row.
        const int cores_rows = narrow ? 1 : (ncores + cpu_cols - 1) / cpu_cols;
        const int mem_h  = 2 + (s.mem.swap_total.value > 0 ? 2 : 1);
        const int net_h  = 2 + ui::NetPanel::rows(s.nets);
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
            graph_h = std::clamp(m.height - fixed, 0, 12);
        } else {
            // Classic split: the top band should FILL the space above a
            // readable process table instead of collapsing to the compact
            // stat-stack height (which left dead sky under the right column).
            // Give the band all the height left after chrome + a min process
            // table, capped so the table keeps a useful number of rows and the
            // graph never balloons absurdly. cpu_h then drives both columns.
            const int chrome = 2 + 3 + 1 + 2;   // header+verdict+footer + proc border
            const int proc_min = 12;            // keep a usable process table
            const int avail = m.height - chrome - proc_min;
            // cpu_h = 3 + graph_h + cores_rows; solve graph_h from the band we
            // want, but never below what the right stack forces (so the CPU
            // column is at least as tall as MEM+NET+DISK compact).
            const int want = std::max(avail, right_stack_h);
            graph_h = std::clamp(want - 3 - cores_rows, 2, 22);
        }
        const int cpu_h  = 2 + 1 + (graph_h >= 2 ? graph_h : 1) + cores_rows;
        const int top_h  = narrow ? cpu_h + mem_h + net_h + disk_h
                                  : std::max(cpu_h, right_stack_h);

        // ── Classic right column: 50 / 50 split, balanced graphs ──
        // The CPU column establishes the band height (rc_target). The right
        // column fills exactly that height in two EQUAL halves (each grows to
        // rc_target/2), and MEMORY and NETWORK carry the SAME-height mountain
        // so neither reads bigger than the other — the graphs are balanced,
        // height-responsive (they scale with the band), and the halves are a
        // strict 50 / 50.
        //   TOP half (50%) = MEMORY
        //   BOT half (50%) = NETWORK + DISK
        const int rc_target   = std::max(cpu_h, right_stack_h);  // band height
        const int rc_top_h    = rc_target / 2;                   // MEMORY half
        const int rc_bot_h    = rc_target - rc_top_h;            // NET+DISK half
        // Each half FILLS with its graph — no dead space, no capping:
        //   MEM graph  = top half minus MEM's meter rows
        //   DISK graph = a small slice of the bottom half
        //   NET graph  = the rest of the bottom half (the big one)
        int rc_mem_graph  = std::max(0, rc_top_h - mem_h);
        const int bot_spare = std::max(0, rc_bot_h - net_h - disk_h);
        int rc_disk_graph = std::min({bot_spare / 4, 4});
        int rc_net_graph  = std::max(0, bot_spare - rc_disk_graph);
        if (rc_mem_graph  < 3) rc_mem_graph  = 0;
        if (rc_net_graph  < 3) rc_net_graph  = 0;
        if (rc_disk_graph < 3) rc_disk_graph = 0;

        // ── Wide 2-column body ──
        // On a big screen the whole thing flips: col 1 stacks EVERY stat panel
        // vertically (CPU · MEM · NET · DISK · TRENDS), col 2 is the process
        // table running the FULL band height beside them. The band spans from
        // just under the verdict banner down to the footer.
        const bool wide2 = m.width >= 200;
        const int band_h = std::max(6, m.height - 5);   // minus header(1)+verdict(3)+footer(1)
        // In wide-2col the proc table owns the band height; otherwise it's the
        // classic strip beneath the top band.
        const int proc_rows = wide2 ? std::max(5, band_h - 2)     // minus panel border
                                    : std::max(5, m.height - 5 - top_h - 2);

        // Process-table inner width. In wide-2col the table lives in the right
        // column, so it's narrower than the full frame; compute it from the
        // same col1 slice the body uses. (col1_w is derived again below; keep
        // the two in sync — both read m.width.)
        const bool wide2_pv = m.width >= 200;
        const int inner_pv = std::max(20, m.width - 2);
        const int col1_pv  = wide2_pv ? std::clamp(inner_pv * 40 / 100, 72, 110) : 0;
        const int proc_inner_w = wide2_pv
            ? std::max(40, inner_pv - col1_pv - 1 - 4)   // minus gap + panel border/pad
            : std::max(20, m.width - 6);

        const ui::OrderedProcs& ord = ordered(m);
        ProcView pv{
            .procs        = ord.procs,
            .sort         = m.sort,
            .sort_desc    = m.sort_desc,
            .selected     = m.sel,
            .scroll       = m.scroll_top,
            .max_rows     = proc_rows,
            .width        = proc_inner_w,
            .filter       = m.filter,
            .filtering    = m.filtering,
            .pending      = m.pending ? &*m.pending : nullptr,
            .tree         = ord.tree,
            .tree_prefix  = ord.prefix,
            .has_kids     = ord.has_kids,
            .collapsed_row = ord.collapsed,
            .hidden_count = ord.hidden,
            .context_row  = ord.context,
            .sub_cpu      = ord.sub_cpu,
            .sub_mem      = ord.sub_mem,
            .sib_share    = ord.sib_share,
            .follow_pid   = m.follow_pid,
        };

        // ── Column split ──
        // Wide 2-col: col 1 (stats, stacked) gets a fixed readable slice, col 2
        // (proc table) takes the rest. Otherwise the classic CPU|stats split.
        const int inner = std::max(20, m.width - 2);      // minus outer padding
        const int gap_w = 1;
        // Stats column width: wide enough for the CPU cores + graphs to breathe
        // but leaving the majority to the process table on an ultra-wide screen.
        const int col1_w = wide2 ? std::clamp(inner * 40 / 100, 72, 110) : 0;
        const int band_inner = wide2 ? col1_w : inner;
        const int right_w = std::clamp((band_inner - gap_w) * 42 / 100, 34, 56);
        const int left_w  = band_inner - gap_w - right_w;
        // Inner content width of a panel = box width - 2 border - 2 padding.
        const int cpu_inner = (narrow ? inner : wide2 ? col1_w - 4 : left_w) - 4;
        const int graph_w = std::max(8, cpu_inner - 4);   // minus y-axis (3) + gap (1)

        // ── Wide 2-col: build col 1 (stacked, graph-forward stats) ──
        if (wide2) {
            // Every panel in col 1 shows a mountain graph. The band height is
            // shared: each panel keeps its fixed content (border + meter/rate
            // rows) and the LEFTOVER is split across the four graphs so the
            // column fills top-to-bottom with no trailing gap.
            // Fixed (non-graph) rows each panel needs:
            const int cpu_fixed  = 2 + 1 + cores_rows;   // border + ALL hdr + cores
            const int mem_fixed  = mem_h;                // border + RAM(+SWP) rows
            const int net_fixed  = net_h;                // border + iface rows
            const int disk_fixed = disk_h;               // border + I/O + mounts
            const int all_fixed  = cpu_fixed + mem_fixed + net_fixed + disk_fixed;
            int graph_pool = band_h - all_fixed;
            // Two goals in tension: FILL the band (no dead gap) but keep each
            // graph READABLE (a 7% line over 20 rows is an empty sky). Resolve
            // it by CAPPING each graph at a comfortable height, then letting
            // the panels grow() to share any leftover band height as breathing
            // room around the (capped) graphs — so col 1 fills top-to-bottom
            // without any single mountain ballooning.
            const bool graph_fill = graph_pool >= 16;
            // Height-responsive FILL: the four graph heights SUM to graph_pool
            // so the four panels stack to EXACTLY band_h — no grow(), so no
            // empty space inside any panel and no gap under DISK. CPU gets the
            // largest slice; net takes the exact remainder so the sum closes.
            int cpu_graph_h  = graph_fill ? std::max(6, graph_pool * 32 / 100) : 0;
            int mem_graph_h  = graph_fill ? std::max(4, graph_pool * 22 / 100) : 0;
            int disk_graph_h = graph_fill ? std::max(4, graph_pool * 22 / 100) : 0;
            int net_graph_h  = graph_fill
                ? std::max(4, graph_pool - cpu_graph_h - mem_graph_h - disk_graph_h) : 0;
            const int cpu_gw = std::max(8, col1_w - 4 - 4 - 4);   // minus y-axis

            Element col1 = v(
                Element{CpuPanel{s.cpu, cpu_cols, cpu_gw, cpu_graph_h, &s.mem}}
                    | hit(ui::hit_band(ui::Detail::Cpu)),
                Element{MemPanel{s.mem, mem_graph_h}}
                    | hit(ui::hit_band(ui::Detail::Mem)),
                Element{NetPanel{s.nets, net_graph_h}}
                    | hit(ui::hit_band(ui::Detail::Net)),
                Element{DiskPanel{s.disks, s.disk_io, false, disk_graph_h}}
                    | grow(1) | hit(ui::hit_band(ui::Detail::Disk))
            ).build();

            Element body = (h(
                std::move(col1) | width(col1_w),
                Element{ProcPanel{s, pv}} | grow(1)
            ) | gap(gap_w) | height(band_h)).build();

            return (v(
                Header{s, m.paused},
                VerdictBanner{s, m.verdict_pulse},
                std::move(body),
                Footer{m.paused, m.ticks, m.toast ? &*m.toast : nullptr,
                       m.pending ? &*m.pending : nullptr, m.filtering, m.filter}
            ) | padding(0, 1, 0, 1)).build();
        }

        Element top = narrow
            ? (v(Element{CpuPanel{s.cpu, cpu_cols, graph_w, graph_h, &s.mem, /*heat=*/true}}
                     | hit(ui::hit_band(ui::Detail::Cpu)),
                 Element{MemPanel{s.mem}}  | hit(ui::hit_band(ui::Detail::Mem)),
                 Element{NetPanel{s.nets}} | hit(ui::hit_band(ui::Detail::Net)),
                 Element{DiskPanel{s.disks, s.disk_io, false}}
                     | hit(ui::hit_band(ui::Detail::Disk)))).build()
            : (h(
                  Element{CpuPanel{s.cpu, cpu_cols, graph_w, graph_h, &s.mem}}
                      | width(left_w) | hit(ui::hit_band(ui::Detail::Cpu)),
                  // The two halves each grow(1): maya's flex distributes the
                  // band's REAL height (forced definite on this column by the
                  // outer row's height(rc_target) + cross-stretch) into two
                  // exact halves, integer-remainder-safe — a true 50/50 at any
                  // terminal size, no hand arithmetic that can drift from the
                  // height maya actually lays the column out at. The graph row
                  // counts (rc_*_graph) are only an ESTIMATE of each half's
                  // interior; grow enforces the split regardless.
                  v(v(Element{MemPanel{s.mem, rc_mem_graph}}
                          | hit(ui::hit_band(ui::Detail::Mem)))
                        | grow(1),
                    v(Element{NetPanel{s.nets, rc_net_graph}}
                          | hit(ui::hit_band(ui::Detail::Net)),
                      Element{DiskPanel{s.disks, s.disk_io, false, rc_disk_graph}}
                          | grow(1) | hit(ui::hit_band(ui::Detail::Disk)))
                        | grow(1))
                    | width(right_w)
              ) | gap(gap_w) | height(rc_target)).build();

        return (v(
            Header{s, m.paused},
            VerdictBanner{s, m.verdict_pulse},
            std::move(top),
            ProcPanel{s, pv},
            Footer{m.paused, m.ticks, m.toast ? &*m.toast : nullptr,
                   m.pending ? &*m.pending : nullptr, m.filtering, m.filter}
        ) | padding(0, 1, 0, 1)).build();
    }
};

}  // namespace rockbottom
