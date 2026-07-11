// widgets/footer.hpp — key hints strip, live indicator, toast notifications,
// and context-sensitive hints (filter mode / kill pending).
//
// Responsive: the strip is a maya fit_row — every hint carries a `keep` rank
// and the row measures its REAL styled fragments, shedding the lowest rank
// first as the terminal narrows. Wide shows everything; narrow keeps the
// hints that matter (q·quit, x·end, /·filter, ?·help) and the status chip.
// Nothing ever clips mid-glyph, and no hand-summed widths can drift.

#pragma once

#include <maya/maya.hpp>

#include "../state.hpp"
#include "../theme.hpp"
#include "hit_ids.hpp"

#include <string>
#include <vector>

namespace rockbottom::ui {

class Footer {
    bool paused_;
    int ticks_;
    const Toast* toast_;
    const PendingKill* pending_;
    bool filtering_;
    std::string filter_;

public:
    Footer(bool paused, int ticks, const Toast* toast,
           const PendingKill* pending, bool filtering, std::string filter)
        : paused_(paused), ticks_(ticks), toast_(toast),
          pending_(pending), filtering_(filtering), filter_(std::move(filter)) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        // A clickable hint carries a hit(id) so the mouse handler resolves
        // it by the SAME rect the renderer painted — no coordinate mirror.
        auto hint = [](const char* k, const char* d) -> Element {
            return (h(
                text(std::string(" ") + k) | nowrap | Bold | fgc(pal::sky),
                text(std::string("·") + d) | nowrap | fgc(pal::dim)
            )).build();
        };
        auto act_hint = [](const char* k, const char* d, FooterAct a) -> Element {
            return (h(
                text(std::string(" ") + k) | nowrap | Bold | fgc(pal::sky),
                text(std::string("·") + d) | nowrap | fgc(pal::dim)
            ) | hit(hit_footer(a))).build();
        };
        // StatusBar idiom: a thin rail separator between logical hint groups
        // so the strip reads as segments, not one long word soup.
        auto sep = []() -> Element {
            return (text(" │") | nowrap | fgc(pal::faint)).build();
        };

        // The strip is a fit_row: each hint carries a `keep` rank and the
        // lowest rank sheds first when the measured row doesn't fit. The
        // separators go first (rank 1), then the label-only niceties, and
        // the strip degrades to q · x · / · ? + the status chip before
        // anything essential is touched. Modal strips (kill / filter) keep
        // their prompt + confirm keys always; only their helper text sheds.
        std::vector<FitItem> parts;

        if (pending_) {
            parts.push_back({(text(" send ") | nowrap | fgc(pal::dim)).build()});
            parts.push_back({(text(sig_name(pending_->sig)) | nowrap | Bold | fgc(pal::hot)).build()});
            parts.push_back({(text(" to " + pending_->name +
                                  (pending_->pids.size() > 1
                                       ? " ×" + std::to_string(pending_->pids.size()) : "") + "? ")
                             | nowrap | fgc(pal::label)).build()});
            parts.push_back({hint("y", "confirm")});
            parts.push_back({hint("n", "cancel")});
        } else if (filtering_) {
            parts.push_back({(text(" filtering: ") | nowrap | fgc(pal::dim)).build()});
            parts.push_back({(text("/" + filter_ + "▌") | nowrap | Bold | fgc(pal::sky)).build()});
            parts.push_back({(text("  user: state: port: cpu: mem: !neg")
                              | nowrap | fgc(pal::faint)).build(), 1});   // syntax cheat — first to go
            parts.push_back({hint("enter", "apply"), 3});
            parts.push_back({hint("esc", "clear"), 2});
        } else {
            // Groups: app │ navigate │ act on process │ view. Only the hints
            // with a real action get a hit id; ↑↓ / 1-6 are labels only.
            // Drop order (first → last): rails · r · ↑↓ · 1-6 · l · t · s ·
            // K · space · / · ? — q·quit and x·end never shed.
            parts.push_back({act_hint("q", "quit", FooterAct::Quit)});          // essential
            parts.push_back({sep(), 1});
            parts.push_back({hint("↑↓", "select"), 2});
            parts.push_back({act_hint("/", "filter", FooterAct::Filter), 7});
            parts.push_back({sep(), 1});
            parts.push_back({act_hint("x", "end", FooterAct::End)});            // essential
            parts.push_back({act_hint("K", "kill", FooterAct::Kill), 5});
            parts.push_back({hint("l", "signal"), 3});
            parts.push_back({hint("r", "nice"), 2});
            parts.push_back({hint("t", "tree"), 4});
            parts.push_back({act_hint("s", "sort", FooterAct::Sort), 4});
            parts.push_back({sep(), 1});
            parts.push_back({hint("1-6", "detail"), 3});
            parts.push_back({act_hint("space", "pause", FooterAct::Pause), 5});
            parts.push_back({act_hint("?", "help", FooterAct::Help), 6});
        }

        // Toast overrides the live indicator on the right.
        Element status;
        if (toast_) {
            Color c = toast_->error ? pal::crit : pal::good;
            status = (text(" " + toast_->text + " ")
                      | nowrap | Bold | fgc(pal::bg) | bgc(c)).build();
        } else if (paused_) {
            status = (text(" ⏸ paused ") | nowrap | Bold | fgc(pal::bg) | bgc(pal::warn)).build();
        } else {
            // Heartbeat: a braille spinner that advances one frame per tick
            // (sysmon-example idiom) — proof of life, not just a static dot.
            static constexpr const char* kSpin[] =
                {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
            status = (h(text(std::string(kSpin[ticks_ % 10]) + " ") | nowrap | fgc(pal::good),
                        text("live " + std::to_string(ticks_)) | nowrap | fgc(pal::dim))).build();
        }

        // Grow spacer (measures 0, always kept) pushes the status chip to
        // the right edge; both ride the fit_row as essentials.
        parts.push_back({Element{space}});
        parts.push_back({std::move(status)});
        parts.push_back({(text(" ") | nowrap).build()});

        return (v(fit_row(std::move(parts), 1))
                | bgc(pal::bg_panel) | padding(0, 1, 0, 1)).build();
    }
};

}  // namespace rockbottom::ui
