// app.cpp — "bottom": a system monitor that answers *what is going on* first,
// and shows the numbers second. Built as a maya Elm-style Program: the Model
// holds the latest Snapshot + UI state, update() is a pure state machine driven
// by a periodic Tick, and view() is a pure function of the Model.
//
// The UX thesis (why this isn't htop/btop):
//   • A big plain-language VERDICT line at the top tells you the situation.
//   • The single loudest process is auto-highlighted, so "what's eating my
//     machine" is answered without reading a 200-row table.
//   • Everything is dimensionally typed underneath, so the numbers are right.

#include <maya/maya.hpp>

#include "sampler.hpp"

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace maya;
using namespace maya::dsl;
using namespace std::chrono_literals;
using namespace bottom;

// ── Palette ─────────────────────────────────────────────────────────────────

namespace pal {
constexpr auto bg_panel = Color::hex(0x181825);
constexpr auto border   = Color::hex(0x313244);
constexpr auto label    = Color::hex(0x9399b2);
constexpr auto dim      = Color::hex(0x6c7086);
constexpr auto text     = Color::hex(0xcdd6f4);
constexpr auto accent   = Color::hex(0x89b4fa);   // blue
constexpr auto good     = Color::hex(0xa6e3a1);   // green
constexpr auto warn     = Color::hex(0xf9e2af);   // yellow
constexpr auto hot      = Color::hex(0xfab387);   // orange
constexpr auto crit     = Color::hex(0xf38ba8);   // red
constexpr auto mauve    = Color::hex(0xcba6f7);
constexpr auto teal     = Color::hex(0x94e2d5);
}  // namespace pal

// gradient green→yellow→orange→red by load fraction.
static Color load_color(double frac) {
    if (frac > 0.90) return pal::crit;
    if (frac > 0.70) return pal::hot;
    if (frac > 0.45) return pal::warn;
    return pal::good;
}

static Color health_color(Health h) {
    switch (h) {
        case Health::Calm:     return pal::good;
        case Health::Busy:     return pal::accent;
        case Health::Stressed: return pal::hot;
        case Health::Critical: return pal::crit;
    }
    return pal::good;
}

static const char* health_glyph(Health h) {
    switch (h) {
        case Health::Calm:     return "●";
        case Health::Busy:     return "◆";
        case Health::Stressed: return "▲";
        case Health::Critical: return "✖";
    }
    return "●";
}

// ── Small render primitives ─────────────────────────────────────────────────

// A smooth meter using eighth-block partial fills — reads as a continuous bar.
static std::string meter(double frac, int width) {
    frac = std::clamp(frac, 0.0, 1.0);
    double cells = frac * width;
    int full = static_cast<int>(cells);
    std::string s;
    for (int i = 0; i < full && i < width; ++i) s += "█";
    if (full < width) {
        static const char* eighths[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
        int frac8 = static_cast<int>((cells - full) * 8);
        s += eighths[std::clamp(frac8, 0, 7)];
        for (int i = full + 1; i < width; ++i) s += "░";
    }
    return s;
}

// Braille-based sparkline from a rolling history window (values 0..1).
static std::string spark(const float* data, int len, int want) {
    static const char* bars[] = {"⠀", "⣀", "⣄", "⣤", "⣦", "⣶", "⣷", "⣿"};
    std::string s;
    int start = len > want ? len - want : 0;
    for (int pad = len; pad < want; ++pad) s += bars[0];
    for (int i = start; i < len; ++i) {
        int idx = std::clamp(static_cast<int>(data[i] * 7.0f), 0, 7);
        s += bars[idx];
    }
    return s;
}

static Element label_text(const std::string& s) { return (text(s) | fgc(pal::label)).build(); }

// A framed panel with a titled rounded border in the house style.
template <class... Cs>
static Element panel(const std::string& title, Cs&&... children) {
    return (v(std::forward<Cs>(children)...)
            | border(BorderStyle::Round)
            | bcolor(pal::border)
            | btext(title, BorderTextPos::Top, BorderTextAlign::Start)
            | padding(0, 1, 0, 1));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Program
// ═════════════════════════════════════════════════════════════════════════════

struct Bottom {
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

    static std::pair<Model, Cmd<Msg>> init() {
        Model m;
        m.snap = m.sampler->sample(m.sort, 40);
        return {std::move(m), Cmd<Msg>{}};
    }

    static std::pair<Model, Cmd<Msg>> update(Model m, Msg msg) {
        return std::visit(overload{
            [&](Tick) {
                if (!m.paused) { ++m.ticks; m.snap = m.sampler->sample(m.sort, 40); }
                return std::pair{std::move(m), Cmd<Msg>{}};
            },
            [&](Resize r)     { m.width = r.w; m.height = r.h; return std::pair{std::move(m), Cmd<Msg>{}}; },
            [&](CycleSort)    { m.sort = static_cast<SortKey>((static_cast<int>(m.sort) + 1) % 4); return resort(std::move(m)); },
            [&](SortCpu)      { m.sort = SortKey::Cpu;  return resort(std::move(m)); },
            [&](SortMem)      { m.sort = SortKey::Mem;  return resort(std::move(m)); },
            [&](SortPid)      { m.sort = SortKey::Pid;  return resort(std::move(m)); },
            [&](SortName)     { m.sort = SortKey::Name; return resort(std::move(m)); },
            [&](TogglePause)  { m.paused = !m.paused; return std::pair{std::move(m), Cmd<Msg>{}}; },
            [&](ToggleHelp)   { m.show_help = !m.show_help; return std::pair{std::move(m), Cmd<Msg>{}}; },
            [&](Quit)         { return std::pair{std::move(m), Cmd<Msg>::quit()}; },
        }, msg);
    }

    static std::pair<Model, Cmd<Msg>> resort(Model m) {
        m.snap = m.sampler->sample(m.sort, 40);
        return {std::move(m), Cmd<Msg>{}};
    }

    static Sub<Msg> subscribe(const Model&) {
        std::vector<Sub<Msg>> subs;
        subs.push_back(Sub<Msg>::every(1s, Tick{}));
        subs.push_back(Sub<Msg>::on_resize([](Size sz) -> Msg {
            return Resize{sz.width.value, sz.height.value};
        }));
        subs.push_back(Sub<Msg>::on_key([](const KeyEvent& ke) -> std::optional<Msg> {
            Event ev{ke};
            if (key(ev, 'q') || key(ev, SpecialKey::Escape)) return Quit{};
            if (key(ev, 'p') || key(ev, ' '))                return TogglePause{};
            if (key(ev, '?') || key(ev, 'h'))                return ToggleHelp{};
            if (key(ev, 's'))                                return CycleSort{};
            if (key(ev, 'c'))                                return SortCpu{};
            if (key(ev, 'm'))                                return SortMem{};
            if (key(ev, 'P'))                                return SortPid{};
            if (key(ev, 'n'))                                return SortName{};
            return std::nullopt;
        }));
        return Sub<Msg>::batch(std::move(subs));
    }

    // ── View ────────────────────────────────────────────────────────────────

    static Element view(const Model& m) {
        const Snapshot& s = m.snap;
        bool narrow = m.width < 92;

        std::vector<Element> root;
        root.push_back(header(m));
        root.push_back(verdict_banner(s));

        // Body: left column (cpu + mem + disk), right column (net + procs).
        Element left = v(cpu_panel(s), mem_panel(s), disk_panel(s)) | grow(1) | gap(0);
        Element right = v(net_panel(s), proc_panel(m)) | grow(1) | gap(0);

        if (narrow) {
            root.push_back((v(std::move(left), std::move(right)) | grow(1)).build());
        } else {
            root.push_back((h(std::move(left), std::move(right)) | grow(1) | gap(1)).build());
        }

        root.push_back(footer(m));
        if (m.show_help) return help_overlay(m);
        return v(root) | padding(0, 1, 0, 1);
    }

    // ── Header ────────────────────────────────────────────────────────────────
    static Element header(const Model& m) {
        const Snapshot& s = m.snap;
        auto pausing = m.paused ? (text("  ⏸ PAUSED") | fgc(pal::warn) | Bold).build()
                                : blank();
        return h(
            text("▓▒░ bottom") | Bold | fgc(pal::mauve),
            text("  " + s.hostname) | fgc(pal::accent) | Bold,
            text("  " + s.kernel) | fgc(pal::dim),
            std::move(pausing),
            space,
            text("up " + humanize_duration(s.uptime_sec)) | fgc(pal::label),
            text("   " + std::to_string(s.proc_count) + " procs") | fgc(pal::dim),
            text(" · " + std::to_string(s.thread_count) + " thr") | fgc(pal::dim)
        ) | padding(0, 0, 0, 0);
    }

    // ── Verdict banner: the headline answer ─────────────────────────────────
    static Element verdict_banner(const Snapshot& s) {
        Color c = health_color(s.verdict.level);
        return h(
            text(std::string(" ") + health_glyph(s.verdict.level) + " ") | fgc(c) | Bold,
            text(s.verdict.headline) | nowrap | Bold | fgc(c),
            text("   " + s.verdict.detail) | nowrap | fgc(pal::label),
            space
        ) | border(BorderStyle::Round) | bcolor(c) | padding(0, 1, 0, 1);
    }

    // ── CPU ─────────────────────────────────────────────────────────────────
    static Element cpu_panel(const Snapshot& s) {
        const CpuInfo& cpu = s.cpu;
        std::vector<Element> rows;

        // Summary line: big total meter + load averages + temp.
        double tf = cpu.total.v;
        char la[64];
        std::snprintf(la, sizeof la, "load %.2f %.2f %.2f",
                      cpu.loadavg[0], cpu.loadavg[1], cpu.loadavg[2]);
        std::vector<Element> summ = {
            (text("ALL ") | fgc(pal::label) | Bold).build(),
            (text(meter(tf, 22)) | fgc(load_color(tf))).build(),
            (text(pct(tf)) | Bold | fgc(load_color(tf))).build(),
        };
        if (cpu.temp_c > 1) {
            Color tc = cpu.temp_c > 80 ? pal::crit : cpu.temp_c > 65 ? pal::hot : pal::teal;
            summ.push_back((text("  " + std::to_string(static_cast<int>(cpu.temp_c)) + "°C") | fgc(tc)).build());
        }
        summ.push_back(space.build());
        summ.push_back((text(la) | fgc(pal::dim)).build());
        rows.push_back((h(summ) | gap(1)).build());

        // Per-core: two columns, ONE compact row each (id · meter · % · spark).
        int n = static_cast<int>(cpu.cores.size());
        int per_col = (n + 1) / 2;
        auto core_cell = [&](int i) -> Element {
            const CpuCore& c = cpu.cores[static_cast<std::size_t>(i)];
            double f = c.usage.v;
            char idbuf[8]; std::snprintf(idbuf, sizeof idbuf, "%2d", i);
            return (h(
                text(idbuf) | fgc(pal::accent) | w_<3>,
                text(meter(f, 9)) | fgc(load_color(f)) | w_<9>,
                text(pct(f)) | fgc(pal::text) | w_<4>,
                text(spark(c.history.data(), c.hist_len, 8)) | nowrap | fgc(pal::dim)
            ) | gap(1)).build();
        };
        for (int r = 0; r < per_col; ++r) {
            std::vector<Element> line;
            line.push_back(core_cell(r));
            int right = per_col + r;
            if (right < n) line.push_back(core_cell(right));
            rows.push_back((h(line) | gap(2)).build());
        }
        return panel(" CPU · " + short_model(cpu.model) + " ", rows);
    }

    // ── Memory ────────────────────────────────────────────────────────────────
    static Element mem_panel(const Snapshot& s) {
        const MemInfo& mem = s.mem;
        double mf = mem.usage().v;
        std::vector<Element> rows;
        rows.push_back((h(
            text("RAM ") | fgc(pal::label) | Bold | w_<5>,
            text(meter(mf, 26)) | nowrap | fgc(load_color(mf)) | w_<26>,
            text(humanize_bytes(mem.used) + " / " + humanize_bytes(mem.total)) | nowrap | fgc(pal::text),
            space,
            text(pct(mf)) | Bold | fgc(load_color(mf))
        ) | gap(1)).build());

        rows.push_back((h(
            text("    cache ") | fgc(pal::dim),
            text(humanize_bytes(mem.cached)) | fgc(pal::teal),
            text("  buffers ") | fgc(pal::dim),
            text(humanize_bytes(mem.buffers)) | fgc(pal::teal),
            text("  avail ") | fgc(pal::dim),
            text(humanize_bytes(mem.available)) | fgc(pal::good)
        )).build());

        if (mem.swap_total.value > 0) {
            double sf = mem.swap_usage().v;
            rows.push_back((h(
                text("SWP ") | fgc(pal::label) | Bold | w_<5>,
                text(meter(sf, 26)) | nowrap | fgc(sf > 0.3 ? pal::crit : pal::mauve) | w_<26>,
                text(humanize_bytes(mem.swap_used) + " / " + humanize_bytes(mem.swap_total)) | nowrap | fgc(pal::text),
                space,
                text(pct(sf)) | fgc(sf > 0.3 ? pal::crit : pal::dim)
            ) | gap(1)).build());
        }
        return panel(" MEMORY ", rows);
    }

    // ── Disk ──────────────────────────────────────────────────────────────────
    static Element disk_panel(const Snapshot& s) {
        std::vector<Element> rows;
        if (s.disks.empty()) rows.push_back(label_text("no mounted disks"));
        for (const auto& d : s.disks) {
            double f = d.usage().v;
            std::string mnt = d.mount.size() > 14 ? "…" + d.mount.substr(d.mount.size() - 13) : d.mount;
            rows.push_back((h(
                text(mnt) | nowrap | fgc(pal::accent) | w_<15>,
                text(meter(f, 14)) | nowrap | fgc(load_color(f)) | w_<14>,
                text(humanize_bytes(d.used) + "/" + humanize_bytes(d.total)) | nowrap | fgc(pal::text) | w_<14>,
                text(pct(f)) | fgc(load_color(f)) | w_<5>,
                text(d.fstype) | fgc(pal::dim)
            ) | gap(1)).build());
        }
        return panel(" DISK ", rows);
    }

    // ── Network ───────────────────────────────────────────────────────────────
    static Element net_panel(const Snapshot& s) {
        std::vector<Element> rows;
        if (s.nets.empty()) rows.push_back(label_text("no active interfaces"));
        for (const auto& n : s.nets) {
            // normalize sparkline against this iface's own recent peak.
            float peak = 1.0f;
            for (int i = 0; i < n.hist_len; ++i) peak = std::max(peak, std::max(n.rx_history[static_cast<std::size_t>(i)], n.tx_history[static_cast<std::size_t>(i)]));
            std::array<float, 48> rxn{}, txn{};
            for (int i = 0; i < n.hist_len; ++i) { rxn[static_cast<std::size_t>(i)] = n.rx_history[static_cast<std::size_t>(i)] / peak; txn[static_cast<std::size_t>(i)] = n.tx_history[static_cast<std::size_t>(i)] / peak; }

            rows.push_back((h(
                text(n.name) | fgc(pal::accent) | Bold | w_<8>,
                text("▼ " + humanize_rate(n.rx)) | fgc(pal::good) | w_<11>,
                text("▲ " + humanize_rate(n.tx)) | fgc(pal::hot) | w_<11>,
                space,
                text(spark(rxn.data(), n.hist_len, 14)) | fgc(pal::good)
            ) | gap(1)).build());
        }
        return panel(" NETWORK ", rows);
    }

    // ── Processes ─────────────────────────────────────────────────────────────
    static Element proc_panel(const Model& m) {
        const Snapshot& s = m.snap;
        std::vector<Element> rows;

        rows.push_back((h(
            text("PID")   | fgc(pal::dim) | Bold | w_<7>,
            text("USER")  | fgc(pal::dim) | Bold | w_<9>,
            text(sort_marked("NAME", m.sort, SortKey::Name)) | fgc(pal::dim) | Bold | w_<16>,
            text(sort_marked("CPU%", m.sort, SortKey::Cpu))  | fgc(pal::dim) | Bold | w_<7>,
            text(sort_marked("MEM", m.sort, SortKey::Mem))   | fgc(pal::dim) | Bold | w_<8>,
            text("THR")   | fgc(pal::dim) | Bold
        ) | gap(1)).build());

        // Which row is the "culprit"? Highlight the loudest by the active key.
        int highlight = -1;
        double best = -1;
        for (std::size_t i = 0; i < s.procs.size(); ++i) {
            double v = m.sort == SortKey::Mem ? static_cast<double>(s.procs[i].rss.value)
                                              : s.procs[i].cpu;
            if (v > best) { best = v; highlight = static_cast<int>(i); }
        }
        bool culprit_loud = (m.sort == SortKey::Mem)
            ? (highlight >= 0 && s.procs[static_cast<std::size_t>(highlight)].mem_share.percent() > 8)
            : (highlight >= 0 && best > 25);

        int cap = std::max(6, m.height - 26);
        for (int i = 0; i < static_cast<int>(s.procs.size()) && i < cap; ++i) {
            const ProcInfo& p = s.procs[static_cast<std::size_t>(i)];
            bool hot = (i == highlight) && culprit_loud;
            Color cpu_c = p.cpu > 50 ? pal::crit : p.cpu > 15 ? pal::hot : pal::text;
            Color name_c = hot ? pal::crit : (p.state == 'R' ? pal::good : pal::text);
            Style name_st = Style{}.with_fg(name_c); if (hot) name_st = name_st.with_bold();
            Style cpu_st  = Style{}.with_fg(cpu_c);  if (p.cpu > 50) cpu_st = cpu_st.with_bold();
            std::string mark = hot ? "»" : " ";
            std::string nm = p.name.size() > 15 ? p.name.substr(0, 14) + "…" : p.name;
            std::string usr = p.user.size() > 8 ? p.user.substr(0, 8) : p.user;

            auto row = h(
                text(mark + std::to_string(p.pid)) | fgc(hot ? pal::crit : pal::dim) | w_<7>,
                text(usr) | nowrap | fgc(pal::label) | w_<9>,
                text(nm, name_st) | nowrap | w_<16>,
                text(cpu1(p.cpu), cpu_st) | w_<7>,
                text(humanize_bytes(p.rss)) | fgc(pal::text) | w_<8>,
                text(std::to_string(p.threads)) | fgc(pal::dim)
            ) | gap(1);
            rows.push_back(row.build());
        }
        return panel(" PROCESSES · sort:" + sort_name(m.sort) + " ", rows) | grow(1);
    }

    // ── Footer / key hints ────────────────────────────────────────────────────
    static Element footer(const Model& m) {
        auto keyhint = [](const char* k, const char* d) {
            return h(text(std::string(" ") + k) | fgc(pal::accent) | Bold,
                     text(std::string(":") + d) | fgc(pal::dim));
        };
        return h(
            keyhint("q", "quit"), keyhint("p", "pause"), keyhint("s", "sort"),
            keyhint("c", "cpu"), keyhint("m", "mem"), keyhint("n", "name"),
            keyhint("P", "pid"), keyhint("?", "help"),
            space,
            text(m.paused ? "paused" : "live") | fgc(m.paused ? pal::warn : pal::good),
            text("  tick " + std::to_string(m.ticks)) | fgc(pal::dim)
        ) | bgc(pal::bg_panel) | padding(0, 1, 0, 1);
    }

    // ── Help overlay ──────────────────────────────────────────────────────────
    static Element help_overlay(const Model&) {
        auto row = [](const char* k, const char* d) {
            return h(text(k) | fgc(pal::accent) | Bold | w_<12>, text(d) | fgc(pal::text));
        };
        return v(
            panel(" bottom · help ",
                text("A calmer system monitor — it tells you what's happening.") | fgc(pal::label),
                blank(),
                row("q / Esc",  "quit"),
                row("p / Space", "pause / resume sampling"),
                row("s",        "cycle sort column"),
                row("c",        "sort by CPU"),
                row("m",        "sort by memory"),
                row("n",        "sort by name"),
                row("P",        "sort by PID"),
                row("? / h",    "toggle this help"),
                blank(),
                text("The banner at the top is the verdict: green is calm,") | fgc(pal::dim),
                text("orange is stressed, red means something needs attention.") | fgc(pal::dim),
                text("The » row is the process driving the current situation.") | fgc(pal::dim)
            ) | width(64)
        ) | align(Align::Center) | justify(Justify::Center) | grow(1) | padding(2);
    }

    // ── Formatting helpers ────────────────────────────────────────────────────
    static std::string pct(double frac) {
        char b[8]; std::snprintf(b, sizeof b, "%d%%", static_cast<int>(std::round(frac * 100))); return b;
    }
    static std::string cpu1(double v) {
        char b[12]; std::snprintf(b, sizeof b, "%.1f", v); return b;
    }
    static std::string sort_name(SortKey k) {
        switch (k) { case SortKey::Cpu: return "cpu"; case SortKey::Mem: return "mem";
                     case SortKey::Pid: return "pid"; case SortKey::Name: return "name"; }
        return "cpu";
    }
    static std::string sort_marked(const char* col, SortKey active, SortKey self) {
        return active == self ? std::string(col) + "▾" : col;
    }
    static std::string short_model(const std::string& m) {
        std::string s = m;
        for (const char* junk : {"(R)", "(TM)", "CPU", "Processor", "  "}) {
            std::size_t p;
            while ((p = s.find(junk)) != std::string::npos) s.erase(p, std::strlen(junk));
        }
        auto a = s.find_first_not_of(' ');
        if (a != std::string::npos) s = s.substr(a);
        if (s.size() > 34) s = s.substr(0, 33) + "…";
        return s.empty() ? "CPU" : s;
    }
};

int main() {
    run<Bottom>({
        .title = "bottom",
        .mouse = false,
        .mode  = Mode::Fullscreen,
    });
    return 0;
}
