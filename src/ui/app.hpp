// app.hpp — the bottom Program: Elm-style Model/Msg/update/view wiring.
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

#include <algorithm>
#include <csignal>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace bottom {

struct App {
    struct Model {
        Snapshot snap;
        SortKey  sort = SortKey::Cpu;
        bool     paused = false;
        bool     show_help = false;
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
    struct Quit {};

    using Msg = std::variant<Tick, Resize, Key, Quit>;

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

        // 4. Normal mode.
        if (key(ev, 'q') || key(ev, maya::SpecialKey::Escape)) {
            if (!m.filter.empty()) { m.filter.clear(); m.sel = 0; return {std::move(m), C{}}; }
            return {std::move(m), C::quit()};
        }
        if (key(ev, 'p') || key(ev, ' '))  { m.paused = !m.paused; return {std::move(m), C{}}; }
        if (key(ev, '?') || key(ev, 'h'))  { m.show_help = true; return {std::move(m), C{}}; }
        if (key(ev, '/'))                  { m.filtering = true; m.filter.clear(); m.sel = 0; return {std::move(m), C{}}; }

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
        return S::batch(std::move(subs));
    }

    // ── view: pure composition of widgets ───────────────────────────────────

    static maya::Element view(const Model& m) {
        using namespace maya;
        using namespace maya::dsl;
        using namespace bottom::ui;

        if (m.show_help) return HelpOverlay{};

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

}  // namespace bottom
