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

// Clip a string to `cap` CELLS, keeping the TAIL and marking the cut with a
// leading ellipsis.
//
// The footer needs this three times and each case has the same reason: the
// END of the string is the part that matters. A filter's caret is at the end;
// an error's reason ("Operation not permitted") is at the end; a truncated
// head still identifies what you're looking at from context. maya ships
// truncate_end but not its mirror, so this is it.
//
// Walks back over whole UTF-8 sequences rather than slicing bytes — a byte
// slice can cut a multi-byte codepoint in half and paint a replacement glyph.
[[nodiscard]] inline std::string clip_head(const std::string& s, int cap) {
    if (cap <= 1 || static_cast<int>(maya::string_width(s)) <= cap) return s;
    const int budget = cap - 1;          // one cell for the ellipsis
    std::size_t i = s.size();
    int cells = 0;
    while (i > 0 && cells < budget) {
        std::size_t j = i - 1;
        while (j > 0 && (static_cast<unsigned char>(s[j]) & 0xC0) == 0x80) --j;
        const int cw = static_cast<int>(maya::string_width(s.substr(j, i - j)));
        if (cells + cw > budget) break;
        cells += cw;
        i = j;
    }
    return "\xe2\x80\xa6" + s.substr(i);
}

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
            // THE KEYS MUST NEVER BE CLIPPED.
            //
            // The process name was unbounded, so a long one pushed "y confirm"
            // and "n cancel" off the right edge: at 50 cols this read
            // "send SIGTERM to fire  y.confir  n.cancel" with the keys cut in
            // half. This is the one strip in the app where a half-rendered
            // instruction is dangerous — it is asking permission to kill
            // something, and the user has to be able to read BOTH answers and
            // see which process they apply to.
            //
            // So the name is clipped, with an ellipsis, to a length that
            // leaves room for both keys even on a 50-column terminal. A fixed
            // cap rather than one derived from the slot width, because fit_row
            // hands each item its own MEASURED width, not the row's — deriving
            // from that is circular and (tried it) truncates "firefox" to six
            // characters on a 200-col screen.
            //
            // 24 is comfortably longer than any real comm name (Linux caps
            // those at 15) while still fitting the narrow case, so in practice
            // this only bites on a synthetic or deliberately silly name.
            // The ranks matter here. Everything is sheddable EXCEPT the two
            // answers: on a 50-column terminal the prose goes before "y" and
            // "n" do, because "SIGTERM firefox?  y confirm  n cancel" is still
            // a complete question, whereas "send SIGTERM to fire  y.confir"
            // is a dialog with half an answer — and this one kills things.
            parts.push_back({(text(" send ") | nowrap | fgc(pal::dim)).build(), 2});
            parts.push_back({(text(sig_name(pending_->sig)) | nowrap | Bold | fgc(pal::hot)).build()});
            {
                const std::string count = pending_->pids.size() > 1
                    ? " \xc3\x97" + std::to_string(pending_->pids.size()) : "";
                const std::string nm{maya::truncate_end(pending_->name, 24)};
                // The target is sheddable too, one rank above the "send" lead:
                // the signal name and the count carry the danger, and a group
                // kill shows "\xc3\x97N" which is the part that must not vanish.
                parts.push_back({(text(" to " + nm + count + "? ")
                                  | nowrap | fgc(pal::label)).build(), 3});
            }
            parts.push_back({hint("y", "confirm")});
            parts.push_back({hint("n", "cancel")});
        } else if (filtering_) {
            // THE QUERY IS THE ONE THING THAT MUST SURVIVE.
            //
            // All four of these used to be fixed parts with the label at
            // keep-0, so on a narrow terminal fit_row kept the word
            // "filtering:" and let the query itself shear off the right edge:
            // at 60 cols a real query rendered as "...cpu:>5 mem:" with the
            // cursor gone. You cannot edit text you cannot see, and this is a
            // live input — it is the single worst thing in the footer to clip.
            //
            // Fixed-width parts, deliberately, so fit_row can MEASURE them and
            // shed the optional ones in a sensible order. (An adaptive
            // component here measures as zero-width, which tells fit_row there
            // is no pressure at all — it then keeps the syntax cheat-sheet and
            // the hints while the real content overflows.)
            //
            // The query is clipped HERE, head-first, keeping the tail where
            // the caret is. The label sheds before anything else, since the
            // leading "/" and the block cursor already say "you are typing".
            //
            // A responsive clip, like the toast below: the caret end must be
            // on screen at EVERY width, and a fixed cap can't promise that —
            // 40 cells of query plus the label and keys still overflows a
            // 50-column strip. fit_row hands this item its own slot, so
            // sizing against that slot is honest here (it is the last
            // flexible thing before the keys).
            parts.push_back({(text(" filtering: ") | nowrap | fgc(pal::dim)).build(), 4});
            parts.push_back({Element{maya::ComponentElement{
                .render = [f = filter_](int slot_w, int) -> Element {
                    using namespace maya; using namespace maya::dsl;
                    return (text("/" + clip_head(f, std::max(6, slot_w - 2)) + "\xe2\x96\x8c")
                            | nowrap | Bold | fgc(pal::sky)).build();
                },
                // Ask for the whole query but accept less; whatever we get,
                // the render above keeps the tail.
                .measure = [f = filter_](int slot_w) -> maya::Size {
                    const int want = static_cast<int>(maya::string_width(f)) + 2;
                    return maya::Size{maya::Columns{std::min(want, std::max(8, slot_w))},
                                      maya::Rows{1}};
                },
            }}});
            parts.push_back({(text("  user: state: port: cpu: mem: !neg")
                              | nowrap | fgc(pal::faint)).build(), 1});   // syntax cheat — first to go
            parts.push_back({hint("enter", "apply"), 3});
            parts.push_back({hint("esc", "clear"), 2});
        } else {
            // Groups: app │ navigate │ act on process │ view. Only the hints
            // with a real action get a hit id; g / 1-7 are labels only.
            // Drop order (first → last): rails · r · g · 1-7 · l · t · s ·
            // K · space · / · ? — q·quit and x·end never shed.
            parts.push_back({act_hint("q", "quit", FooterAct::Quit)});          // essential
            parts.push_back({sep(), 1});
            // g·top over ↑↓·select: the arrows are self-evident in a list
            // (nobody needs telling they move the cursor), whereas jumping
            // back to the busiest process is the thing you actually want
            // after scrolling and can't guess. G is its mirror, documented
            // in `?`.
            parts.push_back({act_hint("g", "top", FooterAct::Top), 2});
            parts.push_back({act_hint("/", "filter", FooterAct::Filter), 7});
            parts.push_back({sep(), 1});
            parts.push_back({act_hint("x", "end", FooterAct::End)});            // essential
            parts.push_back({act_hint("K", "kill", FooterAct::Kill), 5});
            parts.push_back({act_hint("l", "signal", FooterAct::Signal), 3});
            parts.push_back({act_hint("r", "nice", FooterAct::Nice), 2});
            parts.push_back({act_hint("t", "tree", FooterAct::Tree), 4});
            parts.push_back({act_hint("s", "sort", FooterAct::Sort), 4});
            parts.push_back({sep(), 1});
            // 1-7, not 1-6: the USERS pane is the seventh and was left out
            // of this label when it shipped, so the only way to find it was
            // the help screen. A pane nobody can discover may as well not
            // exist. A click opens the FIRST pane (cpu) — the tab bar inside
            // takes you anywhere else, so one click gets you in.
            parts.push_back({act_hint("1-7", "detail", FooterAct::Detail), 3});
            parts.push_back({act_hint("space", "pause", FooterAct::Pause), 5});
            parts.push_back({act_hint("?", "help", FooterAct::Help), 6});
        }

        // Toast overrides the live indicator on the right.
        Element status;
        if (toast_) {
            LitColor c = toast_->error ? pal::crit : pal::good;
            // A long error can be wider than the whole strip — "could not
            // signal 1234: Operation not permitted" is 45 cells and a 50-col
            // terminal has 48 to spend on everything. Shedding every hint
            // still isn't enough, so it has to clip, and WHERE it clips
            // matters: the tail carries the reason ("Operation not
            // permitted"), which is the only actionable part. Head-clipping
            // keeps that and drops the pid, which is already visible in the
            // table you just acted on.
            //
            // Sized against the REAL slot, because this one genuinely needs to
            // know the terminal width — unlike the strips above, a toast is a
            // single item with nothing to its right, so fit_row hands it the
            // remaining row and the measurement isn't circular.
            status = Element{maya::ComponentElement{
                .render = [msg = toast_->text, c](int slot_w, int) -> Element {
                    using namespace maya; using namespace maya::dsl;
                    return (text(" " + clip_head(msg, std::max(8, slot_w - 2)) + " ")
                            | nowrap | Bold | fgc(pal::bg) | bgc(c)).build();
                },
                // Ask for the full message but let the row squeeze us: a
                // smaller grant still paints, head-clipped, rather than
                // overflowing.
                .measure = [msg = toast_->text](int slot_w) -> maya::Size {
                    const int want = static_cast<int>(maya::string_width(msg)) + 2;
                    return maya::Size{maya::Columns{std::min(want, std::max(10, slot_w))},
                                      maya::Rows{1}};
                },
            }};
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
