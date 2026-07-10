// widgets/detail/gpu.hpp — the GPU drill-down body. Replaces nvtop.
//
// Everything nvtop shows and then some, read for you: a utilisation-over-time
// hero graph, core + VRAM meters, a full telemetry strip (temperature, power
// draw vs limit, core/mem clocks, fan, perf-state, encoder/decoder load), the
// processes actually holding VRAM, and a plain-language verdict. Handles
// multiple GPUs, and every vendor field that isn't available is simply omitted.

#pragma once

#include "common.hpp"

namespace rockbottom::ui::detail {

inline std::vector<Element> gpu_body(const Snapshot& s, const Ctx& cx) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> b;

    if (s.gpus.empty()) {
        b.push_back(section("NO GPU DETECTED", pal::proc_ac));
        b.push_back(verdict("no NVIDIA / AMD / Intel GPU telemetry available.", pal::dim));
        b.push_back(verdict("NVIDIA needs the driver + nvidia-smi on PATH; AMD/Intel expose", pal::dim));
        b.push_back(verdict("stats under /sys/class/drm — which may be root-only on your box.", pal::dim));
        return b;
    }

    int gi = 0;
    for (const GpuInfo& g : s.gpus) {
        // ── header ───────────────────────────────────────────────────────────
        std::string head = g.name;
        if (!g.vendor.empty()) head = g.vendor + " · " + head;
        b.push_back((h(
            text(head) | nowrap | Bold | fgc(pal::proc_ac),
            Element{blank()} | grow(1),
            text(g.driver.empty() ? "" : "driver " + g.driver) | nowrap | fgc(pal::dim)
        )).build());
        b.push_back(gap_row());

        // ── hero graph ───────────────────────────────────────────────────
        {
            std::vector<Element> hdr;
            hdr.push_back(Element{section("UTILISATION OVER TIME", pal::proc_ac)} | grow(1));
            hdr.push_back((text("── core ") | nowrap | Bold | fgc(pal::proc_ac)).build());
            hdr.push_back((text(" " + fmt::pct(g.usage.v) + " ") | nowrap | Bold
                           | fgc(pal::bg) | bgc(load_color(g.usage.v))).build());
            hdr.push_back((text(" ── vram ") | nowrap | Bold | fgc(pal::mem_ac)).build());
            hdr.push_back((text(" " + fmt::pct(g.mem_usage.v) + " ") | nowrap | Bold
                           | fgc(pal::bg) | bgc(load_color(g.mem_usage.v))).build());
            b.push_back((h(std::move(hdr)) | gap(1)).build());
        }
        {
            const int gh = std::max(4, cx.graph_h - (s.gpus.size() > 1 ? 3 : 0));
            std::vector<Element> axis;
            for (int r = 0; r < gh; ++r) {
                std::string lbl = r == 0 ? "100" : r == gh - 1 ? "  0" : r == gh / 2 ? " 50" : "   ";
                axis.push_back((text(lbl) | nowrap | fgc(pal::faint)).build());
            }
            Graph gr{g.util_history.data(), g.hist_len};
            gr.fill().rows(gh).color(pal::proc_ac)
              .overlay(g.mem_history.data(), g.mem_hist_len, pal::mem_ac);
            b.push_back((h(
                v(std::move(axis)) | width(3),
                Element{std::move(gr)} | grow(1)
            ) | gap(1) | height(gh)).build());
        }
        b.push_back(gap_row());

        // ── core + VRAM ───────────────────────────────────────────────────────
        b.push_back(section("RIGHT NOW", pal::proc_ac));
        b.push_back(bar("core", g.usage.v, "SM / shader engines busy", load_color(g.usage.v), cx.wide ? 34 : 0));
        b.push_back(bar("vram", g.mem_usage.v,
                        humanize_bytes(g.mem_used) + " / " + humanize_bytes(g.mem_total),
                        pal::mem_ac, cx.wide ? 34 : 0));
        if (g.enc_usage.v > 0 || g.dec_usage.v > 0) {
            b.push_back(bar("encoder", g.enc_usage.v, "NVENC video encode", pal::teal, cx.wide ? 34 : 0));
            b.push_back(bar("decoder", g.dec_usage.v, "NVDEC video decode", pal::sky, cx.wide ? 34 : 0));
        }
        b.push_back(gap_row());

        // ── telemetry strip ────────────────────────────────────────────────────
        b.push_back(section("TELEMETRY", pal::proc_ac));
        b.push_back(kv3(
            "temp", g.temp_c > 1 ? std::to_string(static_cast<int>(g.temp_c)) + " °C" : "n/a",
            load_color(std::clamp((g.temp_c - 40) / 50.0, 0.0, 1.0)),
            "power", g.power_w > 0
                ? fmt::fixed1(g.power_w) + (g.power_limit_w > 0 ? " / " + fmt::fixed1(g.power_limit_w) + "W" : "W")
                : "n/a",
            g.power_limit_w > 0 && g.power_w > g.power_limit_w * 0.9 ? pal::hot : pal::text,
            "fan", g.fan_pct >= 0 ? std::to_string(g.fan_pct) + "%" : "n/a",
            g.fan_pct > 80 ? pal::hot : pal::text));
        b.push_back(kv3(
            "core clock", g.core_clock.value > 0 ? humanize_hz(g.core_clock) : "n/a", pal::proc_ac,
            "mem clock", g.mem_clock.value > 0 ? humanize_hz(g.mem_clock) : "n/a", pal::mem_ac,
            "perf state", g.pstate.empty() ? "n/a" : g.pstate,
            g.pstate == "P0" ? pal::hot : pal::dim));

        // Verdict: read the numbers for you.
        {
            const double u = g.usage.v, mu = g.mem_usage.v;
            std::string msg; maya::Color c;
            if (mu > 0.95) { msg = "▲ VRAM is nearly full — the next allocation may fail or spill to system RAM (slow)"; c = pal::crit; }
            else if (u > 0.9) { msg = "▲ pinned at full load — this is your bottleneck right now"; c = pal::hot; }
            else if (g.temp_c > 84) { msg = "▲ running hot — the card may be thermal-throttling"; c = pal::hot; }
            else if (u < 0.05 && mu < 0.2) { msg = "● idle — nothing is really using the GPU"; c = pal::good; }
            else { msg = "● working normally — comfortable load and thermals"; c = pal::good; }
            b.push_back(verdict(msg, c));
        }
        b.push_back(gap_row());

        // ── VRAM consumers ─────────────────────────────────────────────────────
        b.push_back(section("USING THIS GPU", pal::proc_ac));
        if (g.procs.empty()) {
            b.push_back(verdict("no processes are holding GPU memory right now", pal::dim));
        } else {
            const int show = std::min<int>(cx.tall ? 8 : 5, static_cast<int>(g.procs.size()));
            for (int i = 0; i < show; ++i) {
                const GpuProc& p = g.procs[static_cast<std::size_t>(i)];
                const double frac = g.mem_total.value ? Ratio::of(p.mem, g.mem_total).v : 0;
                b.push_back(rank_row(i + 1, std::to_string(p.pid), std::string(fmt::clip(p.name, 22)),
                                     frac, pal::proc_ac,
                                     humanize_bytes(p.mem), pal::proc_ac, 10));
            }
        }

        if (++gi < static_cast<int>(s.gpus.size())) {
            b.push_back(gap_row());
            b.push_back((text(std::string(cx.w - 4, '-')) | nowrap | fgc(pal::faint)).build());
            b.push_back(gap_row());
        }
    }

    return b;
}

}  // namespace rockbottom::ui::detail
