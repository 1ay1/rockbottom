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
#include "../state.hpp"
#include "../theme.hpp"
#include "detail_kind.hpp"
#include "hit_ids.hpp"
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

namespace rockbottom::ui {

// Detail enum now lives in detail_kind.hpp (included above) so lightweight
// headers can name it without the full pane stack.

class DetailPane {
    const Snapshot& s_;
    Detail which_;
    const ProcInfo* proc_;   // for Detail::Proc
    int w_, h_, scroll_;
    const PendingKill* pending_ = nullptr;  // in-pane kill confirmation

public:
    DetailPane(const Snapshot& s, Detail which, const ProcInfo* proc = nullptr,
               int w = 100, int h = 40, int scroll = 0,
               const PendingKill* pending = nullptr)
        : s_(s), which_(which), proc_(proc), w_(w), h_(h), scroll_(scroll),
          pending_(pending) {}

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
        // The scroller is a fill component (grow baked in) — it takes the
        // rows left between the sysbar and the hint. On ultrawide the domain
        // panes reflow into two_col (which caps its OWN column widths), so
        // the scroller must NOT re-clamp them — hand those the full slot; a
        // single-column body (proc, or any pane below ultrawide) gets the
        // readable design-width cap so its graph/meters don't smear.
        const bool split_body = cx.ultrawide && which_ != Detail::Proc;
        framed.push_back(detail::scroller(std::move(rows), cx.scroll,
                                          cx.body_h, ac, /*cap_width=*/!split_body));
        framed.push_back(pending_ ? confirm_strip() : hint());

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
            case Detail::Gpu:  glyph = "◆"; title = "GPU";     ac = pal::gpu_ac;  break;
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
        // Census with lit figures — a faint mush of text hides the one number
        // you came for; ink the counts, keep the joinery quiet.
        std::vector<Element> census;
        auto fig = [&](const std::string& n, const char* unit, maya::Color c) {
            census.push_back((text(n) | nowrap | Bold | fgc(c)).build());
            census.push_back((text(unit) | nowrap | fgc(pal::faint)).build());
        };
        auto dot = [&] { census.push_back((text("  ·  ") | nowrap | fgc(pal::faint)).build()); };
        fig(std::to_string(s_.proc_count), " procs", pal::label);
        dot();
        fig(std::to_string(s_.thread_count), " threads", pal::label);
        dot();
        fig(std::to_string(s_.running), " running", pal::good);
        if (s_.zombies) { dot(); fig(std::to_string(s_.zombies), " zombie", pal::hot); }
        if (s_.dstate)  { dot(); fig(std::to_string(s_.dstate), " blocked", pal::crit); }
        std::string batt;
        if (s_.battery.present)
            batt = "  \xf0\x9f\x94\x8b " + std::to_string(s_.battery.percent) + "%" + (s_.battery.charging ? " \xe2\x86\x91" : "");
        // The top identity row is width-aware: hostname + kernel on the left,
        // uptime + battery on the right, a flex spacer between. At narrow
        // widths a plain h-stack of nowrap cells has no slack in the spacer,
        // so the fixed cells BUTT together ("7.1.3-zeup 1d" — kernel fused into
        // "up"). Build it as a component that measures the real width and
        // sheds from the left: full kernel → clipped kernel → kernel dropped,
        // always preserving a 2-cell gap before the right group.
        const std::string host = s_.hostname;
        const std::string kern = s_.kernel;
        const std::string upt = "up " + humanize_duration(s_.uptime_sec);
        Element id_row = Element{maya::ComponentElement{
            .render = [host, kern, upt, batt](int w, int) -> Element {
                using namespace maya; using namespace maya::dsl;
                const int hostw = static_cast<int>(string_width(host));
                const int rightw = static_cast<int>(string_width(upt))
                                 + static_cast<int>(string_width(batt));
                // Cells that must always show: host (left) + up/batt (right)
                // + a 2-cell minimum gap. Whatever's left after that is the
                // kernel's budget; below ~6 cells it's not worth showing.
                const int kern_room = w - hostw - rightw - 4;
                std::string k;
                if (kern_room >= 6)
                    k = "  " + std::string(fmt::clip(kern,
                            static_cast<std::size_t>(kern_room - 2)));
                std::vector<Element> row;
                row.push_back((text(host) | nowrap | Bold | fgc(pal::white)).build());
                if (!k.empty())
                    row.push_back((text(k) | nowrap | fgc(pal::dim)).build());
                row.push_back((Element{blank()} | grow(1)).build());
                row.push_back((text(upt) | nowrap | fgc(pal::label)).build());
                if (!batt.empty())
                    row.push_back((text(batt) | nowrap | fgc(pal::good)).build());
                return (h(std::move(row))).build();
            },
        }};
        return (v(
            std::move(id_row),
            h(std::move(census)),
            blank()
        )).build();
    }

    Element hint() const {
        using namespace maya; using namespace maya::dsl;
        // The number keys switch domain, so render them as what they are: a
        // TAB BAR. The pane you're in is lit in its own accent + underline;
        // the rest sit quiet, so "where am I / where can I go" is one glance.
        //
        // Responsive via maya pick() (ViewThatFits): the same bar is built at
        // three densities — glyph chips + labels, keys + labels tightened,
        // keys only — and the first whose MEASURED width fits the pane
        // renders. No breakpoints; a renamed tab re-decides by itself.
        struct Tab { Detail d; const char* key; const char* glyph; const char* label; maya::Color ac; };
        const Tab tabs[] = {
            {Detail::Cpu,  "1", "◈", "cpu",  pal::cpu_ac},
            {Detail::Mem,  "2", "▤", "mem",  pal::mem_ac},
            {Detail::Net,  "3", "⇅", "net",  pal::net_ac},
            {Detail::Gpu,  "4", "◆", "gpu",  pal::gpu_ac},
            {Detail::Disk, "5", "◇", "disk", pal::disk_ac},
            {Detail::Proc, "6", "≡", "proc", pal::proc_ac},
        };
        const bool scrollable = content_rows() > viewport_rows();

        // density 2 = chips + labels · 1 = tighter, keys + labels · 0 = keys.
        auto bar = [&](int density) -> Element {
            const char* pad = density == 2 ? "   " : density == 1 ? "  " : " ";
            std::vector<Element> row;
            row.push_back((text(" esc") | nowrap | Bold | fgc(pal::sky)).build());
            if (density > 0)
                row.push_back((text("·back") | nowrap | fgc(pal::dim)).build());
            row.push_back((text(pad) | nowrap).build());
            for (const Tab& t : tabs) {
                const bool on = which_ == t.d;
                if (on) {
                    // The pane you're IN is a solid chip — dark ink on the
                    // tab's accent, the footer status-chip idiom. One filled
                    // block among quiet labels is unmissable at a glance.
                    std::string chip = density == 2
                        ? " " + std::string(t.glyph) + " " + t.key + " " + t.label + " "
                        : density == 1
                        ? " " + std::string(t.key) + " " + t.label + " "
                        : " " + std::string(t.key) + " ";
                    row.push_back((text(std::move(chip))
                                   | nowrap | Bold | fgc(pal::bg) | bgc(t.ac)
                                   | hit(hit_tab(t.d))).build());
                } else if (density > 0) {
                    row.push_back((h(
                        text(t.key) | nowrap | Bold | fgc(pal::sky),
                        text(" " + std::string(t.label)) | nowrap | fgc(pal::dim)
                    ) | hit(hit_tab(t.d))).build());
                } else {
                    row.push_back((text(t.key) | nowrap | Bold | fgc(pal::sky)
                                   | hit(hit_tab(t.d))).build());
                }
                row.push_back((text(pad) | nowrap).build());
            }
            if (scrollable && density == 2) {
                row.push_back((Element{blank()} | grow(1)).build());
                row.push_back((text("↑↓") | nowrap | Bold | fgc(pal::sky)).build());
                row.push_back((text("·scroll") | nowrap | fgc(pal::dim)).build());
            }
            return (h(std::move(row))).build();
        };

        return (v(pick({bar(2), bar(1), bar(0)}))).build();
    }

    // In-pane kill confirmation — the same y/n contract the process table uses,
    // surfaced at the foot of the detail pane so a kill armed from here (x / K
    // / X / T) shows its prompt instead of silently waiting.
    Element confirm_strip() const {
        using namespace maya; using namespace maya::dsl;
        const auto& p = *pending_;
        const bool hard = p.sig == SIGKILL;
        const bool group = p.pids.size() > 1;
        const bool lethal = p.sig == SIGKILL || p.sig == SIGTERM ||
                            p.sig == SIGQUIT || p.sig == SIGABRT || p.sig == SIGINT;
        maya::Color c = hard ? pal::crit : lethal ? pal::warn : pal::sky;
        std::string what = group
            ? std::to_string(p.pids.size()) + " procs · " + p.name
            : p.name + " (" + std::to_string(p.pid) + ")";
        std::string verb =
              p.sig == SIGKILL ? "force-kill "
            : p.sig == SIGTERM ? "end "
            : (p.sig == SIGSTOP) ? "suspend "
            : p.sig == SIGCONT ? "resume "
            : "send " + sig_name(p.sig) + " to ";
        return (h(
            text(" " + verb + what + "? ") | nowrap | Bold | fgc(pal::bg) | bgc(c),
            text("  y") | nowrap | Bold | fgc(pal::good),
            text("·confirm  ") | nowrap | fgc(pal::dim),
            text("n") | nowrap | Bold | fgc(pal::crit),
            text("·cancel") | nowrap | fgc(pal::dim)
        )).build();
    }
};

}  // namespace rockbottom::ui
