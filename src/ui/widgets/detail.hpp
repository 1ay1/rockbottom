// widgets/detail.hpp — the DETAIL PANE: a full-screen, breathtaking drill-down
// into one domain (CPU / MEM / NET / DISK / a single process). This is where
// bottom pulls ahead of htop & btop — every number gets room to breathe, with
// big graphs, real breakdowns, and plain-language read-outs instead of a wall
// of digits. Opened with 1-5 (or Enter on a process / a click on a panel
// title); Esc closes.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../fmt.hpp"
#include "../theme.hpp"
#include "panel.hpp"
#include "graph.hpp"
#include "spark.hpp"
#include "meter.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace bottom::ui {

enum class Detail { None, Cpu, Mem, Net, Disk, Proc };

class DetailPane {
    const Snapshot& s_;
    Detail which_;
    const ProcInfo* proc_;   // for Detail::Proc

public:
    DetailPane(const Snapshot& s, Detail which, const ProcInfo* proc = nullptr)
        : s_(s), which_(which), proc_(proc) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        Element card;
        switch (which_) {
            case Detail::Cpu:  card = cpu_detail();  break;
            case Detail::Mem:  card = mem_detail();  break;
            case Detail::Net:  card = net_detail();  break;
            case Detail::Disk: card = disk_detail(); break;
            case Detail::Proc: card = proc_detail(); break;
            default:           card = blank();       break;
        }
        // Full-screen takeover — fill every cell so nothing shows behind.
        return (v(std::move(card) | grow(1)) | grow(1)).build();
    }

private:
    using Element = maya::Element;

    // A label : value row, label dim + left, value bold + colored.
    static Element kv(const std::string& k, const std::string& v, maya::Color vc) {
        using namespace maya; using namespace maya::dsl;
        return (h(
            text(k) | nowrap | fgc(pal::dim) | width(16),
            text(v) | nowrap | Bold | fgc(vc)
        )).build();
    }

    // A full-width labelled meter row: "label  ██████░░  42%  <tail>".
    static Element bar(const std::string& label, double frac,
                       const std::string& tail, maya::Color c) {
        using namespace maya; using namespace maya::dsl;
        return (h(
            text(label) | nowrap | fgc(pal::label) | width(14),
            Element{Meter{frac}.fill().color(c)} | grow(1),
            text(fmt::pct_pad(frac)) | nowrap | Bold | fgc(load_color(frac)) | width(5) | justify(Justify::End),
            text(tail) | nowrap | fgc(pal::dim) | width(34)
        ) | gap(2)).build();
    }

    static Element section(const char* title, maya::Color ac) {
        using namespace maya; using namespace maya::dsl;
        return (text(title) | nowrap | Bold | fgc(ac)).build();
    }

    static Element hint() {
        using namespace maya; using namespace maya::dsl;
        return (h(
            text(" esc") | nowrap | Bold | fgc(pal::sky),
            text("·back  ") | nowrap | fgc(pal::dim),
            text("1") | nowrap | Bold | fgc(pal::sky),
            text(" cpu ") | nowrap | fgc(pal::dim),
            text("2") | nowrap | Bold | fgc(pal::sky),
            text(" mem ") | nowrap | fgc(pal::dim),
            text("3") | nowrap | Bold | fgc(pal::sky),
            text(" net ") | nowrap | fgc(pal::dim),
            text("4") | nowrap | Bold | fgc(pal::sky),
            text(" disk ") | nowrap | fgc(pal::dim),
            text("5") | nowrap | Bold | fgc(pal::sky),
            text(" proc") | nowrap | fgc(pal::dim)
        )).build();
    }

    // Wrap a body in the house Panel, occupying the whole screen.
    Element frame(const char* glyph, const std::string& title, maya::Color ac,
                  std::vector<Element> body) const {
        using namespace maya; using namespace maya::dsl;
        // Push the hint to the very bottom: a growing spacer eats the slack.
        body.push_back((Element{blank()} | grow(1)).build());
        body.push_back(hint());
        return Panel(glyph, title, ac).grow(1)(std::move(body));
    }

    // ── CPU ──────────────────────────────────────────────────────────────
    Element cpu_detail() const {
        using namespace maya; using namespace maya::dsl;
        const CpuInfo& c = s_.cpu;
        std::vector<Element> b;

        b.push_back(section("LOAD OVER TIME", pal::cpu_ac));
        // Big graph with a real y-axis.
        {
            std::vector<Element> axis;
            for (int r = 0; r < 8; ++r) {
                const char* lbl = r == 0 ? "100" : r == 7 ? "  0" : r == 4 ? " 50" : "   ";
                axis.push_back((text(lbl) | nowrap | fgc(pal::faint)).build());
            }
            b.push_back((h(
                v(std::move(axis)) | width(3),
                Element{Graph{c.total_history.data(), c.total_hist_len}.fill().rows(8)} | grow(1)
            ) | gap(1) | height(8)).build());
        }
        b.push_back(blank());

        b.push_back(section("RIGHT NOW", pal::cpu_ac));
        b.push_back(bar("total", c.total.v, "busy across all cores", load_color(c.total.v)));
        b.push_back(bar("iowait", c.iowait.v, "waiting on disk", pal::hot));
        b.push_back(kv("load 1/5/15", fmt::fixed2(c.loadavg[0]) + "  " +
                       fmt::fixed2(c.loadavg[1]) + "  " + fmt::fixed2(c.loadavg[2]),
                       load_color(std::min(1.0, c.loadavg[0] / std::max(1, c.logical)))));
        b.push_back(kv("logical cpus", std::to_string(c.logical), pal::text));
        if (c.temp_c > 1)
            b.push_back(kv("package temp", std::to_string(static_cast<int>(c.temp_c)) + " °C",
                           load_color(std::clamp((c.temp_c - 40) / 50.0, 0.0, 1.0))));
        b.push_back(blank());

        b.push_back(section("PER-CORE", pal::cpu_ac));
        // Two columns of cores, each a full labelled meter.
        const int n = static_cast<int>(c.cores.size());
        const int cols = n > 16 ? 4 : n > 8 ? 2 : 1;
        const int per = (n + cols - 1) / cols;
        for (int r = 0; r < per; ++r) {
            std::vector<Element> line;
            for (int col = 0; col < cols; ++col) {
                int i = col * per + r;
                if (i >= n) { line.push_back(Element{blank()} | grow(1)); continue; }
                const CpuCore& core = c.cores[static_cast<std::size_t>(i)];
                const double f = core.usage.v;
                char id[8]; std::snprintf(id, sizeof id, "%2d", i);
                std::string fq = core.freq.value > 0
                    ? " " + fmt::fixed2(static_cast<double>(core.freq.value) / 1e9) + "G" : "";
                line.push_back(Element{(h(
                    text(id) | nowrap | fgc(pal::cpu_ac) | width(3),
                    Element{Meter{f}.fill().groove(false)} | grow(1),
                    text(fmt::pct_pad(f)) | nowrap | fgc(load_color(f)) | width(5) | justify(Justify::End),
                    text(fq) | nowrap | fgc(pal::faint) | width(6)
                ) | gap(1)).build()} | grow(1));
            }
            b.push_back((h(line) | gap(3)).build());
        }

        return frame("◈", "CPU · " + fmt::short_model(c.model), pal::cpu_ac, std::move(b));
    }

    // ── MEMORY ───────────────────────────────────────────────────────────
    Element mem_detail() const {
        using namespace maya; using namespace maya::dsl;
        const MemInfo& m = s_.mem;
        std::vector<Element> b;

        b.push_back(section("USAGE TREND", pal::mem_ac));
        {
            std::vector<Element> axis;
            for (int r = 0; r < 7; ++r) {
                const char* lbl = r == 0 ? "100" : r == 6 ? "  0" : "   ";
                axis.push_back((text(lbl) | nowrap | fgc(pal::faint)).build());
            }
            b.push_back((h(
                v(std::move(axis)) | width(3),
                Element{Graph{m.usage_history.data(), m.hist_len}.fill().rows(7).color(pal::mem_ac)} | grow(1)
            ) | gap(1) | height(7)).build());
        }
        b.push_back(blank());

        b.push_back(section("PHYSICAL", pal::mem_ac));
        b.push_back(bar("used", m.usage().v, humanize_bytes(m.used) + " / " + humanize_bytes(m.total), pal::mem_ac));
        const double cache_f = Ratio::of(m.cached, m.total).v;
        b.push_back(bar("cache", cache_f, humanize_bytes(m.cached) + " (reclaimable)", pal::teal));
        const double buf_f = Ratio::of(m.buffers, m.total).v;
        b.push_back(bar("buffers", buf_f, humanize_bytes(m.buffers), pal::sky));
        b.push_back(kv("available", humanize_bytes(m.available), pal::good));
        b.push_back(blank());

        b.push_back(section("SWAP", pal::mem_ac));
        if (m.swap_total.value > 0) {
            b.push_back(bar("used", m.swap_usage().v, humanize_bytes(m.swap_used) + " / " + humanize_bytes(m.swap_total), pal::hot));
            b.push_back(kv("paging in",  humanize_rate(m.swap_in),  m.swap_in.per_sec  > 1024 ? pal::crit : pal::dim));
            b.push_back(kv("paging out", humanize_rate(m.swap_out), m.swap_out.per_sec > 1024 ? pal::crit : pal::dim));
            const bool thrash = m.swap_in.per_sec + m.swap_out.per_sec > 4096;
            b.push_back((text(thrash ? "  ▲ actively thrashing — the machine wants more RAM"
                                     : "  ● swap is parked; nothing to worry about")
                         | fgc(thrash ? pal::crit : pal::good)).build());
        } else {
            b.push_back((text("  no swap configured") | fgc(pal::dim)).build());
        }
        b.push_back(blank());

        // PSI memory pressure.
        if (s_.psi.mem.available) {
            b.push_back(section("PRESSURE (PSI · last 10s)", pal::mem_ac));
            b.push_back(bar("some stalled", s_.psi.mem.some_avg10 / 100.0,
                            "≥1 task waited on memory", pal::hot));
            b.push_back(bar("full stalled", s_.psi.mem.full_avg10 / 100.0,
                            "everything waited", pal::crit));
        }

        return frame("▤", "MEMORY", pal::mem_ac, std::move(b));
    }

    // ── NETWORK ──────────────────────────────────────────────────────────
    Element net_detail() const {
        using namespace maya; using namespace maya::dsl;
        std::vector<Element> b;

        if (s_.nets.empty()) {
            b.push_back((text("  no active interfaces") | fgc(pal::dim)).build());
            return frame("⇅", "NETWORK", pal::net_ac, std::move(b));
        }

        for (const NetIface& ni : s_.nets) {
            b.push_back((h(
                text(ni.name) | nowrap | Bold | fgc(pal::net_ac) | width(12),
                text(ni.up ? "● up" : "○ down") | nowrap | fgc(ni.up ? pal::good : pal::dim)
            ) | gap(1)).build());
            // rx / tx sparkline pair, full width.
            b.push_back((h(
                text("  ▼ rx") | nowrap | fgc(pal::sky) | width(7),
                Element{Spark{ni.rx_history.data(), ni.hist_len}.fill().color(pal::sky)} | grow(1),
                text(humanize_rate(ni.rx)) | nowrap | Bold | fgc(pal::sky) | width(12) | justify(Justify::End)
            ) | gap(1)).build());
            b.push_back((h(
                text("  ▲ tx") | nowrap | fgc(pal::good) | width(7),
                Element{Spark{ni.tx_history.data(), ni.hist_len}.fill().color(pal::good)} | grow(1),
                text(humanize_rate(ni.tx)) | nowrap | Bold | fgc(pal::good) | width(12) | justify(Justify::End)
            ) | gap(1)).build());
            b.push_back((h(
                text("  totals") | nowrap | fgc(pal::dim) | width(9),
                text("↓ " + humanize_bytes(ni.rx_total) + "   ↑ " + humanize_bytes(ni.tx_total))
                    | nowrap | fgc(pal::label)
            )).build());
            b.push_back(blank());
        }

        return frame("⇅", "NETWORK", pal::net_ac, std::move(b));
    }

    // ── DISK ─────────────────────────────────────────────────────────────
    Element disk_detail() const {
        using namespace maya; using namespace maya::dsl;
        std::vector<Element> b;

        b.push_back(section("SYSTEM I/O", pal::disk_ac));
        b.push_back((h(
            text("  ▼ read ") | nowrap | fgc(pal::teal) | width(9),
            Element{Spark{s_.disk_io.read_history.data(), s_.disk_io.hist_len}.fill().color(pal::teal)} | grow(1),
            text(humanize_rate(s_.disk_io.read)) | nowrap | Bold | fgc(pal::teal) | width(12) | justify(Justify::End)
        ) | gap(1)).build());
        b.push_back((h(
            text("  ▲ write") | nowrap | fgc(pal::hot) | width(9),
            Element{Spark{s_.disk_io.write_history.data(), s_.disk_io.hist_len}.fill().color(pal::hot)} | grow(1),
            text(humanize_rate(s_.disk_io.write)) | nowrap | Bold | fgc(pal::hot) | width(12) | justify(Justify::End)
        ) | gap(1)).build());
        if (s_.psi.io.available)
            b.push_back(bar("io pressure", s_.psi.io.some_avg10 / 100.0,
                            "% of last 10s stalled on I/O", pal::hot));
        b.push_back(blank());

        b.push_back(section("FILESYSTEMS", pal::disk_ac));
        // Column header.
        b.push_back((h(
            text("mount") | nowrap | Bold | fgc(pal::dim) | width(16),
            text("usage") | nowrap | Bold | fgc(pal::dim) | grow(1),
            text("used / size") | nowrap | Bold | fgc(pal::dim) | width(20) | justify(Justify::End),
            text("  fs") | nowrap | Bold | fgc(pal::dim) | width(8)
        ) | gap(1)).build());
        for (const DiskInfo& d : s_.disks) {
            const double f = d.usage().v;
            b.push_back((h(
                text(fmt::clip(d.mount, 15)) | nowrap | fgc(pal::text) | width(16),
                Element{Meter{f}.fill().color(pal::disk_ac)} | grow(1),
                text(fmt::pct_pad(f)) | nowrap | Bold | fgc(load_color(f)) | width(5) | justify(Justify::End),
                text(humanize_bytes(d.used) + " / " + humanize_bytes(d.total))
                    | nowrap | fgc(pal::label) | width(15) | justify(Justify::End),
                text("  " + d.fstype) | nowrap | fgc(pal::dim) | width(8)
            ) | gap(1)).build());
        }

        return frame("◇", "DISK", pal::disk_ac, std::move(b));
    }

    // ── SINGLE PROCESS ───────────────────────────────────────────────────
    Element proc_detail() const {
        using namespace maya; using namespace maya::dsl;
        std::vector<Element> b;
        if (!proc_) {
            b.push_back((text("  no process selected") | fgc(pal::dim)).build());
            return frame("≡", "PROCESS", pal::proc_ac, std::move(b));
        }
        const ProcInfo& p = *proc_;

        b.push_back((h(
            text(std::to_string(p.pid)) | nowrap | Bold | fgc(pal::proc_ac) | width(10),
            text(p.name) | nowrap | Bold | fgc(pal::white)
        )).build());
        b.push_back((text("  " + (p.cmd.empty() ? p.name : p.cmd)) | fgc(pal::dim)).build());
        b.push_back(blank());

        b.push_back(section("RESOURCES", pal::proc_ac));
        b.push_back(bar("cpu", std::clamp(p.cpu / 100.0, 0.0, 1.0),
                        fmt::fixed1(p.cpu) + "% of one core", load_color(std::clamp(p.cpu / 100.0, 0.0, 1.0))));
        b.push_back(bar("memory", p.mem_share.v,
                        humanize_bytes(p.rss) + " resident", pal::mem_ac));
        b.push_back(blank());

        b.push_back(section("I/O & THREADS", pal::proc_ac));
        const double ior = p.io_read.per_sec, iow = p.io_write.per_sec;
        b.push_back(kv("disk read",  ior > 1 ? humanize_rate(p.io_read)  : "idle", ior > 512 ? pal::sky : pal::dim));
        b.push_back(kv("disk write", iow > 1 ? humanize_rate(p.io_write) : "idle", iow > 512 ? pal::sky : pal::dim));
        b.push_back(kv("threads", std::to_string(p.threads), pal::text));
        b.push_back(blank());

        b.push_back(section("STATE", pal::proc_ac));
        const char* st = p.state == 'R' ? "running"
                       : p.state == 'S' ? "sleeping (interruptible)"
                       : p.state == 'D' ? "◆ uninterruptible — blocked on I/O"
                       : p.state == 'Z' ? "zombie"
                       : p.state == 'T' ? "stopped" : "unknown";
        maya::Color sc = p.state == 'R' ? pal::good : p.state == 'D' ? pal::crit
                       : p.state == 'Z' ? pal::hot  : pal::dim;
        b.push_back(kv("run state", st, sc));
        b.push_back(kv("owner", p.user, pal::label));

        if (!p.ports.empty()) {
            b.push_back(blank());
            b.push_back(section("LISTENING PORTS", pal::proc_ac));
            std::string ports;
            for (std::size_t i = 0; i < p.ports.size() && i < 24; ++i)
                ports += (i ? "  " : "") + (":" + std::to_string(p.ports[i]));
            b.push_back((text("  " + ports) | fgc(pal::sky)).build());
        }

        b.push_back(blank());
        b.push_back((h(
            text("  x") | nowrap | Bold | fgc(pal::warn), text("·end  ") | nowrap | fgc(pal::dim),
            text("K") | nowrap | Bold | fgc(pal::crit), text("·force-kill") | nowrap | fgc(pal::dim)
        )).build());

        return frame("≡", "PROCESS", pal::proc_ac, std::move(b));
    }
};

}  // namespace bottom::ui
