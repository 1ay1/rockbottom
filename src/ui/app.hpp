// app.hpp — the bottom Program: Elm-style Model/Msg/update/view wiring.
// All rendering is delegated to the ui::* widgets; all data collection to
// core::Sampler. This file is just the state machine and layout composition.

#pragma once

#include <maya/maya.hpp>

#include "../core/sampler.hpp"
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

#include <memory>
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
        std::shared_ptr<Sampler> sampler = std::make_shared<Sampler>();
    };

    struct Tick {};
    struct Resize { int w, h; };
    struct CycleSort {};
    struct SortCpu {}; struct SortMem {}; struct SortPid {}; struct SortName {};
    struct TogglePause {};
    struct ToggleHelp {};
    struct Quit {};

    using Msg = std::variant<Tick, Resize, CycleSort, SortCpu, SortMem, SortPid,
                             SortName, TogglePause, ToggleHelp, Quit>;

    // ── init / update / subscribe ───────────────────────────────────────────

    static std::pair<Model, maya::Cmd<Msg>> init() {
        Model m;
        m.snap = m.sampler->sample(m.sort, 40);
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    static std::pair<Model, maya::Cmd<Msg>> resample(Model m) {
        m.snap = m.sampler->sample(m.sort, 40);
        return {std::move(m), maya::Cmd<Msg>{}};
    }

    static std::pair<Model, maya::Cmd<Msg>> update(Model m, Msg msg) {
        using C = maya::Cmd<Msg>;
        return std::visit(maya::overload{
            [&](Tick) {
                if (!m.paused) { ++m.ticks; m.snap = m.sampler->sample(m.sort, 40); }
                return std::pair{std::move(m), C{}};
            },
            [&](Resize r)    { m.width = r.w; m.height = r.h; return std::pair{std::move(m), C{}}; },
            [&](CycleSort)   { m.sort = static_cast<SortKey>((static_cast<int>(m.sort) + 1) % 4); return resample(std::move(m)); },
            [&](SortCpu)     { m.sort = SortKey::Cpu;  return resample(std::move(m)); },
            [&](SortMem)     { m.sort = SortKey::Mem;  return resample(std::move(m)); },
            [&](SortPid)     { m.sort = SortKey::Pid;  return resample(std::move(m)); },
            [&](SortName)    { m.sort = SortKey::Name; return resample(std::move(m)); },
            [&](TogglePause) { m.paused = !m.paused; return std::pair{std::move(m), C{}}; },
            [&](ToggleHelp)  { m.show_help = !m.show_help; return std::pair{std::move(m), C{}}; },
            [&](Quit)        { return std::pair{std::move(m), C::quit()}; },
        }, msg);
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
            maya::Event ev{ke};
            using maya::key;
            if (key(ev, 'q') || key(ev, maya::SpecialKey::Escape)) return Quit{};
            if (key(ev, 'p') || key(ev, ' '))                      return TogglePause{};
            if (key(ev, '?') || key(ev, 'h'))                      return ToggleHelp{};
            if (key(ev, 's'))                                      return CycleSort{};
            if (key(ev, 'c'))                                      return SortCpu{};
            if (key(ev, 'm'))                                      return SortMem{};
            if (key(ev, 'P'))                                      return SortPid{};
            if (key(ev, 'n'))                                      return SortName{};
            return std::nullopt;
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

        // Right column gets the process table sized to the remaining height:
        // total - header(1) - banner(3) - net panel - footer(1) - borders.
        const int net_rows = static_cast<int>(s.nets.size()) + 2;
        const int proc_rows = std::max(6, m.height - net_rows - 9);

        Element left = (v(
            CpuPanel{s.cpu},
            MemPanel{s.mem},
            DiskPanel{s.disks}
        ) | width(64)).build();

        Element right = (v(
            NetPanel{s.nets},
            ProcPanel{s, m.sort, proc_rows}
        ) | grow(1)).build();

        Element body = narrow
            ? (v(std::move(left), std::move(right)) | grow(1)).build()
            : (h(std::move(left), std::move(right)) | grow(1) | gap(1)).build();

        return (v(
            Header{s, m.paused},
            VerdictBanner{s},
            std::move(body),
            Footer{m.paused, m.ticks}
        ) | padding(0, 1, 0, 1)).build();
    }
};

}  // namespace bottom
