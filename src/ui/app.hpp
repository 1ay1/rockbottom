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
#include <csignal>
#include <memory>
#include <optional>
#include <set>
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
        std::string filter;              // active name filter ("" = off)
        bool        filtering = false;   // '/' typing mode
        bool        sort_desc = true;    // sort direction (▼ default, ▲ reversed)
        bool        tree = false;        // process-tree view vs flat sorted list
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
        L.cpu_cols = ncores > 24 ? 4 : ncores > 12 ? 3 : 2;
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
            // The graph absorbs ALL the height the right stack forces on the
            // CPU column — no artificial cap, so a tall NETWORK panel means a
            // taller mountain, never dead space under the cores.
            L.graph_h = std::max(2, right_stack_h - 3 - cores_rows);
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

    // First visible process index given the scroll window (mirrors
    // ProcPanel::build()'s clamp exactly so click→index is never off).
    static int scroll_start(int sel, int n, int body_rows) {
        int start = 0;
        if (sel >= body_rows) start = sel - body_rows + 1;
        return std::clamp(start, 0, std::max(0, n - body_rows));
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
            case FooterAct::Filter: m.filtering = true; m.filter.clear(); m.sel = 0; return {std::move(m), C{}};
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
        // The very first sample runs synchronously: there is no event loop yet
        // to block, and the first frame should paint with real data instead of
        // an empty snapshot. Every sample AFTER this is a background effect.
        m.snap = m.sampler->sample(m.sort, 400);
        return {std::move(m), maya::Cmd<Msg>{}};
    }

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
                dispatch(Sampled{sampler->sample(sort, 400)});
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
            [&](Quit)     { return std::pair{std::move(m), C::quit()}; },
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
                if (key(ev, maya::SpecialKey::Enter)) {
                    m.detail = ui::Detail::None; m.detail_scroll = 0; m.detail_pid = 0;
                    return {std::move(m), C{}};
                }
                if (key(ev, 'x') || key(ev, maya::SpecialKey::Delete)) return arm_kill(std::move(m), SIGTERM);
                if (key(ev, 'K')) return arm_kill(std::move(m), SIGKILL);
                if (key(ev, 'X')) return arm_kill_all(std::move(m), SIGTERM);
                if (key(ev, 'l')) return open_sigmenu(std::move(m));
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

        // Selection.
        if (key(ev, maya::SpecialKey::Down) || key(ev, 'j')) { ++m.sel; clamp_sel(m); m.follow_pid = 0; return {std::move(m), C{}}; }
        if (key(ev, maya::SpecialKey::Up)   || key(ev, 'k')) { --m.sel; clamp_sel(m); m.follow_pid = 0; return {std::move(m), C{}}; }
        if (key(ev, maya::SpecialKey::Home)) { m.sel = 0; m.follow_pid = 0; return {std::move(m), C{}}; }
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

    // The fully-ordered process view (filter → sort/tree). This is the SINGLE
    // source of row order: the panel renders it and every index-based action
    // (selection, kill, follow, collapse) resolves against the same vector.
    static ui::OrderedProcs ordered(const Model& m) {
        return ui::order_procs(m.snap.procs, m.filter, m.sort, m.sort_desc,
                               m.tree, m.collapsed);
    }

    static std::vector<const ProcInfo*> filtered(const Model& m) {
        return ordered(m).procs;
    }

    static void clamp_sel(Model& m) {
        int n = static_cast<int>(filtered(m).size());
        m.sel = std::clamp(m.sel, 0, std::max(0, n - 1));
    }

    // The pid under the cursor in the current ordered view, or 0 if none.
    static int selected_pid(const Model& m) {
        auto view = filtered(m);
        if (!view.empty() && m.sel >= 0 && m.sel < static_cast<int>(view.size()))
            return view[static_cast<std::size_t>(m.sel)]->pid;
        return 0;
    }

    // Re-point the selection at a given pid after the list re-orders (toggling
    // tree, sort, collapse). Keeps the cursor on the SAME process instead of
    // the same index, which is the difference between "solid" and "jumpy".
    static void select_pid(Model& m, int pid) {
        if (pid <= 0) return;
        auto view = filtered(m);
        for (std::size_t i = 0; i < view.size(); ++i)
            if (view[i]->pid == pid) { m.sel = static_cast<int>(i); return; }
        clamp_sel(m);
    }

    // Toggle the process-tree view, keeping the cursor on the same process.
    static std::pair<Model, maya::Cmd<Msg>> toggle_tree(Model m) {
        const int keep = selected_pid(m);
        m.tree = !m.tree;
        if (!m.tree) m.collapsed.clear();   // fresh slate when leaving tree mode
        select_pid(m, keep);
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Collapse (fold=true) or expand (fold=false) the selected subtree. In
    // flat mode this is a no-op. Collapsing a leaf jumps to its parent so
    // ←/→ feels like real tree navigation (htop idiom).
    static std::pair<Model, maya::Cmd<Msg>> set_collapse(Model m, bool fold) {
        if (!m.tree) return {std::move(m), maya::Cmd<Msg>{}};
        ui::OrderedProcs ord = ordered(m);
        if (m.sel < 0 || m.sel >= static_cast<int>(ord.procs.size()))
            return {std::move(m), maya::Cmd<Msg>{}};
        const int pid = ord.procs[static_cast<std::size_t>(m.sel)]->pid;
        const bool kids = m.sel < static_cast<int>(ord.has_kids.size())
                          && ord.has_kids[static_cast<std::size_t>(m.sel)];
        if (fold) {
            if (kids && !m.collapsed.count(pid)) { m.collapsed.insert(pid); }
            else {
                // Already a leaf or already folded → hop to the parent so a
                // repeated ← walks up the tree.
                const int ppid = ord.procs[static_cast<std::size_t>(m.sel)]->ppid;
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
        auto view = filtered(m);
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
        auto view = filtered(m);
        if (!view.empty() && m.sel < static_cast<int>(view.size())) {
            const ProcInfo* p = view[static_cast<std::size_t>(m.sel)];
            m.sigmenu = Model::SigMenu{p->pid, p->name, {p->pid}, 0};
        }
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    // Open the renice dial on the selected process, seeded with its current
    // nice value so ←→ nudge from where it already is.
    static std::pair<Model, maya::Cmd<Msg>> open_nicemenu(Model m) {
        auto view = filtered(m);
        if (!view.empty() && m.sel < static_cast<int>(view.size())) {
            const ProcInfo* p = view[static_cast<std::size_t>(m.sel)];
            m.nicemenu = Model::NiceMenu{p->pid, p->name, p->nice, p->nice};
        }
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    static std::pair<Model, maya::Cmd<Msg>> arm_kill(Model m, int sig) {
        auto view = filtered(m);
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
        auto view = filtered(m);
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
        fold(m.paused ? 1 : 0);
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
            return DetailPane{m.snap, m.detail, p, m.width, m.height, m.detail_scroll};
        }

        const Snapshot& s = m.snap;
        const bool narrow = m.width < 96;

        // ── Height budget ──
        // Fixed rows: header(1) + verdict(3) + footer(1) + outer padding.
        // Top band: CPU panel (graph 4 + blank + cores) vs MEM+NET+DISK stack.
        const int ncores = static_cast<int>(s.cpu.cores.size());
        const int cpu_cols = ncores > 24 ? 4 : ncores > 12 ? 3 : 2;   // stay ~8 rows tall
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
            // cpu_h = 2(border) + 1(ALL header) + graph_h + cores_rows. Solve
            // for the graph_h that makes cpu_h == right_stack_h — uncapped,
            // so the graph soaks up ALL the height the right stack forces.
            graph_h = std::max(2, right_stack_h - 3 - cores_rows);
        }
        const int cpu_h  = 2 + 1 + (graph_h >= 2 ? graph_h : 1) + cores_rows;
        const int top_h  = narrow ? cpu_h + mem_h + net_h + disk_h
                                  : std::max(cpu_h, right_stack_h);
        const int proc_rows = std::max(5, m.height - 5 - top_h - 2);

        ui::OrderedProcs ord = ordered(m);
        ProcView pv{
            .procs        = ord.procs,
            .sort         = m.sort,
            .sort_desc    = m.sort_desc,
            .selected     = m.sel,
            .max_rows     = proc_rows,
            .width        = std::max(20, m.width - 6),
            .filter       = m.filter,
            .filtering    = m.filtering,
            .pending      = m.pending ? &*m.pending : nullptr,
            .tree         = ord.tree,
            .tree_prefix  = ord.prefix,
            .has_kids     = ord.has_kids,
            .collapsed_row = ord.collapsed,
            .hidden_count = ord.hidden,
            .follow_pid   = m.follow_pid,
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
            ? (v(Element{CpuPanel{s.cpu, cpu_cols, graph_w, graph_h, &s.mem, /*heat=*/true}}
                     | hit(ui::hit_band(ui::Detail::Cpu)),
                 Element{MemPanel{s.mem}}  | hit(ui::hit_band(ui::Detail::Mem)),
                 Element{NetPanel{s.nets}} | hit(ui::hit_band(ui::Detail::Net)),
                 Element{DiskPanel{s.disks, s.disk_io, false}}
                     | hit(ui::hit_band(ui::Detail::Disk)))).build()
            : (h(
                  Element{CpuPanel{s.cpu, cpu_cols, graph_w, graph_h, &s.mem}}
                      | width(left_w) | hit(ui::hit_band(ui::Detail::Cpu)),
                  v(Element{MemPanel{s.mem}}  | hit(ui::hit_band(ui::Detail::Mem)),
                    Element{NetPanel{s.nets}} | hit(ui::hit_band(ui::Detail::Net)),
                    Element{DiskPanel{s.disks, s.disk_io, false}}
                        | hit(ui::hit_band(ui::Detail::Disk))) | width(right_w)
              ) | gap(gap_w)).build();

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
