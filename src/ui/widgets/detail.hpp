// widgets/detail.hpp — the DETAIL PANE dispatcher.
//
// A full-screen, per-domain drill-down that carries MORE detail than htop or
// btop: every number gets room to breathe, graphs get a real y-axis, and every
// pane ends in a plain-language verdict. The heavy lifting lives in the
// per-domain files under detail/ — this header just picks the right one, wraps
// it in the house frame with a system strip + hint bar, and runs the SCROLLER
// so a dense pane is always usable at any terminal size.
//
// Opened with 1-5 (or Enter on a process / a click on a panel title). Esc
// closes. In a scrollable pane ↑↓ / PgUp / PgDn / Home / End move the window.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../fmt.hpp"
#include "../theme.hpp"
#include "panel.hpp"

#include "detail/common.hpp"
#include "detail/cpu.hpp"
#include "detail/mem.hpp"
#include "detail/net.hpp"
#include "detail/gpu.hpp"
#include "detail/disk.hpp"
#include "detail/proc.hpp"

#include <string>
#include <vector>

namespace bottom::ui {

enum class Detail { None, Cpu, Mem, Net, Gpu, Disk, Proc };

class DetailPane {
    const Snapshot& s_;
    Detail which_;
    const ProcInfo* proc_;   // for Detail::Proc
    int w_, h_, scroll_;

public:
    DetailPane(const Snapshot& s, Detail which, const ProcInfo* proc = nullptr,
               int w = 100, int h = 40, int scroll = 0)
        : s_(s), which_(which), proc_(proc), w_(w), h_(h), scroll_(scroll) {}

    operator maya::Element() const { return build(); }

    // How many body rows the current pane produces — the app uses this to clamp
    // the scroll offset so you can't scroll past the end.
    [[nodiscard]] int content_rows() const {
        return static_cast<int>(body().size());
    }

    // Rows visible in the framed viewport (== terminal height minus chrome).
    [[nodiscard]] int viewport_rows() const {
        return detail::Ctx::make(w_, h_, scroll_).body_h;
    }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        const char* glyph; std::string title; maya::Color ac;
        meta(glyph, title, ac);

        detail::Ctx cx = detail::Ctx::make(w_, h_, scroll_);
        std::vector<Element> rows = body();

        // Frame: system strip on top, scrollable body in the middle, hint at
        // the bottom. The scroller clips `rows` to the viewport and draws a
        // scrollbar when there's more than fits.
        std::vector<Element> framed;
        framed.push_back(sysbar());
        framed.push_back(detail::scroller(std::move(rows), cx.scroll, cx.body_h, ac));
        framed.push_back(hint());

        Element card = Panel(glyph, title, ac).grow(1)(std::move(framed));
        return (v(std::move(card) | grow(1)) | grow(1)).build();
    }

private:
    using Element = maya::Element;

    void meta(const char*& glyph, std::string& title, maya::Color& ac) const {
        switch (which_) {
            case Detail::Cpu:  glyph = "◈"; title = "CPU · " + fmt::short_model(s_.cpu.model); ac = pal::cpu_ac; break;
            case Detail::Mem:  glyph = "▤"; title = "MEMORY";  ac = pal::mem_ac;  break;
            case Detail::Net:  glyph = "⇅"; title = "NETWORK"; ac = pal::net_ac;  break;
            case Detail::Gpu:  glyph = "◆"; title = "GPU";     ac = pal::proc_ac; break;
            case Detail::Disk: glyph = "◇"; title = "DISK";    ac = pal::disk_ac; break;
            case Detail::Proc: glyph = "≡"; title = "PROCESS"; ac = pal::proc_ac; break;
            default:           glyph = " "; title = "";        ac = pal::text;    break;
        }
    }

    std::vector<Element> body() const {
        detail::Ctx cx = detail::Ctx::make(w_, h_, scroll_);
        switch (which_) {
            case Detail::Cpu:  return detail::cpu_body(s_, cx);
            case Detail::Mem:  return detail::mem_body(s_, cx);
            case Detail::Net:  return detail::net_body(s_, cx);
            case Detail::Gpu:  return detail::gpu_body(s_, cx);
            case Detail::Disk: return detail::disk_body(s_, cx);
            case Detail::Proc: return detail::proc_body(s_, cx, proc_);
            default:           return {};
        }
    }

    // System context strip shared by every pane — host / kernel / uptime and
    // the process census, so you always know which machine you're looking at.
    Element sysbar() const {
        using namespace maya; using namespace maya::dsl;
        std::string census = std::to_string(s_.proc_count) + " procs · " +
                             std::to_string(s_.thread_count) + " threads · " +
                             std::to_string(s_.running) + " running";
        if (s_.zombies) census += " · " + std::to_string(s_.zombies) + " zombie";
        if (s_.dstate)  census += " · " + std::to_string(s_.dstate) + " blocked";
        std::string batt;
        if (s_.battery.present)
            batt = "  🔋 " + std::to_string(s_.battery.percent) + "%" + (s_.battery.charging ? " ↑" : "");
        return (v(
            h(
                text(s_.hostname) | nowrap | Bold | fgc(pal::white),
                text("  " + s_.kernel) | nowrap | fgc(pal::dim),
                Element{blank()} | grow(1),
                text("up " + humanize_duration(s_.uptime_sec)) | nowrap | fgc(pal::label),
                text(batt) | nowrap | fgc(pal::good)
            ),
            text(census) | nowrap | fgc(pal::faint),
            blank()
        )).build();
    }

    Element hint() const {
        using namespace maya; using namespace maya::dsl;
        const bool scrollable = content_rows() > viewport_rows();
        std::vector<Element> row;
        row.push_back((text(" esc") | nowrap | Bold | fgc(pal::sky)).build());
        row.push_back((text("·back  ") | nowrap | fgc(pal::dim)).build());
        row.push_back((text("1") | nowrap | Bold | fgc(pal::sky)).build());
        row.push_back((text(" cpu ") | nowrap | fgc(pal::dim)).build());
        row.push_back((text("2") | nowrap | Bold | fgc(pal::sky)).build());
        row.push_back((text(" mem ") | nowrap | fgc(pal::dim)).build());
        row.push_back((text("3") | nowrap | Bold | fgc(pal::sky)).build());
        row.push_back((text(" net ") | nowrap | fgc(pal::dim)).build());
        row.push_back((text("4") | nowrap | Bold | fgc(pal::sky)).build());
        row.push_back((text(" gpu ") | nowrap | fgc(pal::dim)).build());
        row.push_back((text("5") | nowrap | Bold | fgc(pal::sky)).build());
        row.push_back((text(" disk ") | nowrap | fgc(pal::dim)).build());
        row.push_back((text("6") | nowrap | Bold | fgc(pal::sky)).build());
        row.push_back((text(" proc") | nowrap | fgc(pal::dim)).build());
        if (scrollable) {
            row.push_back((Element{blank()} | grow(1)).build());
            row.push_back((text("↑↓") | nowrap | Bold | fgc(pal::sky)).build());
            row.push_back((text("·scroll") | nowrap | fgc(pal::dim)).build());
        }
        return (h(std::move(row))).build();
    }
};

}  // namespace bottom::ui
