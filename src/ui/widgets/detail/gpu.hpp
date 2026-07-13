// widgets/detail/gpu.hpp — the GPU drill-down body. Replaces nvtop.
//
// Works for every vendor rockbottom samples — Apple Silicon (IOAccelerator:
// device/renderer/tiler utilisation, unified memory, core count), NVIDIA
// (nvidia-smi: SM %, VRAM, temp, power vs limit, clocks, fan, NVENC/NVDEC,
// P-state, per-process VRAM), AMD (sysfs: busy %, VRAM, hwmon telemetry,
// clocks) and Intel (clocks + hwmon). Every field a vendor can't report is
// simply omitted — never a fake zero dressed up as data — so the pane is
// honest and dense on every platform.

#pragma once

#include "common.hpp"

namespace rockbottom::ui::detail {

inline std::vector<Element> gpu_body(const Snapshot& s, const Ctx& cx) {
    using namespace maya; using namespace maya::dsl;

    // In ultrawide mode a SINGLE-GPU pane splits into two side-by-side columns.
    // `L` collects the left (header + utilisation graph + engines + telemetry +
    // verdict), `R` the right (memory + processes holding VRAM). With more than
    // one GPU we keep the single-column stack so each card reads top-to-bottom
    // and the inter-GPU separator still works. In normal mode both point at the
    // same vector.
    std::vector<Element> single;
    std::vector<Element> hero, left, right;
    const bool split = cx.ultrawide && s.gpus.size() == 1;
    std::vector<Element>& L = split ? left : single;
    std::vector<Element>& R = split ? right : single;
    // Header + hero graph ride the full pane width in split mode.
    std::vector<Element>& H = split ? hero : single;

    if (s.gpus.empty()) {
        single.push_back(section("NO GPU DETECTED", pal::gpu_ac));
        single.push_back(verdict("no GPU telemetry available on this machine.", pal::dim));
        single.push_back(verdict("NVIDIA needs the driver + nvidia-smi on PATH; AMD/Intel expose", pal::dim));
        single.push_back(verdict("stats under /sys/class/drm; Apple GPUs publish IOAccelerator nodes.", pal::dim));
        return single;
    }

    int gi = 0;
    for (const GpuInfo& g : s.gpus) {
        // ── header: vendor · name, core count, driver ─────────────────────
        std::string head = g.name;
        if (!g.vendor.empty() && g.name.find(g.vendor) == std::string::npos)
            head = g.vendor + " · " + head;
        std::string sub;
        if (g.cores > 0) sub += std::to_string(g.cores) + "-core";
        if (g.unified) sub += sub.empty() ? "unified memory" : " · unified memory";
        if (!g.driver.empty()) sub += (sub.empty() ? "" : " · ") + ("driver " + g.driver);
        H.push_back((h(
            text(head) | nowrap | Bold | fgc(pal::gpu_ac),
            Element{blank()} | grow(1),
            text(sub) | nowrap | fgc(pal::dim)
        )).build());
        H.push_back(gap_row());

        // ── hero graph ─────────────────────────────────────────────────────
        // ── hero: BIG number + graph ─────────────────────────────────
        {
            std::vector<Element> hdr;
            hdr.push_back(Element{section("UTILISATION OVER TIME", pal::gpu_ac)} | grow(1));
            hdr.push_back((text("── core ") | nowrap | Bold | fgc(pal::gpu_ac)).build());
            hdr.push_back((text(" ── vram ") | nowrap | Bold | fgc(pal::mem_ac)).build());
            H.push_back((h(std::move(hdr)) | gap(1)).build());
        }
        {
            const int gh = std::max(4, cx.graph_h - (s.gpus.size() > 1 ? 3 : 0));
            H.push_back(hero_graph(g.usage.v, load_color(g.usage.v), "gpu busy",
                                   g.util_history.data(), g.hist_len, gh,
                                   pal::gpu_ac,
                                   g.mem_history.data(), g.mem_hist_len, pal::mem_ac));
        }
        if (!split) L.push_back(gap_row());

        // ── engines ────────────────────────────────────────────────────────
        // Every engine the card reports, one meter each. Apple splits the
        // pipeline into renderer (3D/fragment) and tiler (geometry); NVIDIA
        // reports NVENC/NVDEC video engines.
        L.push_back(section("ENGINES", pal::gpu_ac));
        L.push_back(bar("core", g.usage.v, "overall GPU busy", load_color(g.usage.v), cx.wide ? 34 : 0));
        // Gate the extra engine rows on the VENDOR, not the live value — a
        // renderer at 0% must render as an empty meter, not vanish, or the
        // pane's layout breathes with the workload.
        if (g.vendor == "Apple") {
            L.push_back(bar("renderer", g.renderer_usage.v, "3D / fragment work", pal::gpu_ac, cx.wide ? 34 : 0));
            L.push_back(bar("tiler", g.tiler_usage.v, "geometry / vertex work", pal::sky, cx.wide ? 34 : 0));
        }
        if (g.vendor == "NVIDIA") {
            L.push_back(bar("encoder", g.enc_usage.v, "video encode (NVENC)", pal::teal, cx.wide ? 34 : 0));
            L.push_back(bar("decoder", g.dec_usage.v, "video decode (NVDEC)", pal::sky, cx.wide ? 34 : 0));
        }
        L.push_back(gap_row());

        // ── memory ──────────────────────────────────────────────────
        R.push_back(section("MEMORY", pal::gpu_ac));
        if (g.mem_total.value) {
            R.push_back(bar("vram", g.mem_usage.v,
                            humanize_bytes(g.mem_used) + " / " + humanize_bytes(g.mem_total),
                            pal::mem_ac, cx.wide ? 34 : 0));
            if (g.unified)
                R.push_back(verdict("unified memory — the GPU shares system RAM, so GPU allocations "
                                    "compete with your apps", pal::dim));
        } else if (g.mem_used.value) {
            R.push_back(kv3(
                "in use", humanize_bytes(g.mem_used), pal::mem_ac,
                "", "", pal::dim, "", "", pal::dim));
        } else {
            R.push_back(verdict("this driver does not report memory usage", pal::dim));
        }
        R.push_back(gap_row());

        // ── telemetry strip — only rows that carry at least one live figure ──
        {
            struct Cell { std::string k, v; maya::Color c; };
            std::vector<Cell> cells;
            if (g.temp_c > 1)
                cells.push_back({"temp", std::to_string(static_cast<int>(g.temp_c)) + " °C",
                                 load_color(std::clamp((g.temp_c - 40) / 50.0, 0.0, 1.0))});
            if (g.power_w > 0)
                cells.push_back({"power",
                                 std::to_string(static_cast<int>(g.power_w + 0.5)) + "W" +
                                     (g.power_limit_w > 0
                                          ? " / " + std::to_string(static_cast<int>(g.power_limit_w + 0.5)) + "W"
                                          : ""),
                                 g.power_limit_w > 0 && g.power_w > g.power_limit_w * 0.9 ? pal::hot : pal::text});
            if (g.fan_pct >= 0)
                cells.push_back({"fan", std::to_string(g.fan_pct) + "%",
                                 g.fan_pct > 80 ? pal::hot : pal::text});
            if (g.core_clock.value > 0)
                cells.push_back({"core clock", humanize_hz(g.core_clock), pal::gpu_ac});
            if (g.mem_clock.value > 0)
                cells.push_back({"mem clock", humanize_hz(g.mem_clock), pal::mem_ac});
            if (!g.pstate.empty()) {
                const bool resets = g.pstate.find("reset") != std::string::npos;
                cells.push_back({resets ? "gpu resets" : "perf state", g.pstate,
                                 resets ? pal::hot
                                 : g.pstate == "P0" ? pal::hot : pal::dim});
            }
            if (!cells.empty()) {
                L.push_back(section("TELEMETRY", pal::gpu_ac));
                for (std::size_t i = 0; i < cells.size(); i += 3) {
                    auto at = [&](std::size_t j) -> const Cell* {
                        return j < cells.size() ? &cells[j] : nullptr;
                    };
                    const Cell* c1 = at(i);
                    const Cell* c2 = at(i + 1);
                    const Cell* c3 = at(i + 2);
                    L.push_back(kv3(
                        c1 ? c1->k : "", c1 ? c1->v : "", c1 ? c1->c : pal::dim,
                        c2 ? c2->k : "", c2 ? c2->v : "", c2 ? c2->c : pal::dim,
                        c3 ? c3->k : "", c3 ? c3->v : "", c3 ? c3->c : pal::dim));
                }
            }
        }

        // Verdict: read the numbers for you.
        {
            const double u = g.usage.v, mu = g.mem_usage.v;
            std::string msg; maya::Color c;
            if (!g.unified && mu > 0.95) { msg = "▲ VRAM is nearly full — the next allocation may fail or spill to system RAM (slow)"; c = pal::crit; }
            else if (g.unified && mu > 0.5) { msg = "▲ the GPU holds over half of system RAM — apps and GPU are fighting for memory"; c = pal::hot; }
            else if (u > 0.9) { msg = "▲ pinned at full load — this is your bottleneck right now"; c = pal::hot; }
            else if (g.temp_c > 84) { msg = "▲ running hot — the card may be thermal-throttling"; c = pal::hot; }
            else if (u < 0.05 && mu < 0.2) { msg = "● idle — nothing is really using the GPU"; c = pal::good; }
            else { msg = "● working normally — comfortable load and thermals"; c = pal::good; }
            L.push_back(verdict(msg, c));
        }
        L.push_back(gap_row());

        // ── GPU processes ───────────────────────────────────────────
        // Not just "who holds VRAM" — who's actually WORKING the GPU. Each row
        // gets a type badge (C compute · G graphics · B both), the VRAM it
        // holds, and — where the backend attributes it per-process — its live
        // GPU-compute %. Sorted so the busiest (or biggest) rise to the top.
        if (!g.procs.empty()) {
            const bool any_util = std::any_of(g.procs.begin(), g.procs.end(),
                [](const GpuProc& p) { return p.has_util; });
            const int show = std::min<int>(cx.tall ? 8 : 5, static_cast<int>(g.procs.size()));
            R.push_back(section("USING THIS GPU", pal::gpu_ac,
                                std::string("top ") + std::to_string(show) +
                                (any_util ? " · gpu% · vram" : " · vram")));
            for (int i = 0; i < show; ++i) {
                const GpuProc& p = g.procs[static_cast<std::size_t>(i)];
                const double frac = g.mem_total.value ? Ratio::of(p.mem, g.mem_total).v : 0;
                const char badge = p.type == 'B' ? 'B' : p.type == 'G' ? 'G'
                                 : p.type == 'C' ? 'C' : ' ';
                std::string nm = maya::truncate_end(p.name, 20);
                if (badge != ' ') nm = std::string(1, badge) + " " + nm;
                // Primary value = VRAM. When utilisation is known, the meter
                // tracks GPU-busy (what's *working* the card) and the util %
                // rides the v2 column; otherwise the meter tracks VRAM share.
                if (any_util) {
                    const std::string util = p.has_util
                        ? fmt::pct(p.sm.v) : "—";
                    R.push_back(rank_row(i + 1, std::to_string(p.pid), nm,
                                         p.has_util ? p.sm.v : frac,
                                         p.sm.v > 0.5 ? pal::hot : pal::gpu_ac,
                                         util, p.sm.v > 0.5 ? pal::hot : pal::label, 5,
                                         std::string(humanize_bytes(p.mem)), pal::gpu_ac, 9));
                } else {
                    R.push_back(rank_row(i + 1, std::to_string(p.pid), nm,
                                         frac, pal::gpu_ac,
                                         std::string(humanize_bytes(p.mem)), pal::gpu_ac, 10));
                }
            }
        }

        if (++gi < static_cast<int>(s.gpus.size())) {
            single.push_back(gap_row());
            single.push_back((text(std::string(static_cast<std::size_t>(std::max(8, cx.w - 4)), '-'))
                         | nowrap | fgc(pal::faint)).build());
            single.push_back(gap_row());
        }
    }

    if (split) return hero_split(std::move(hero), std::move(left), std::move(right));
    return single;
}

}  // namespace rockbottom::ui::detail
