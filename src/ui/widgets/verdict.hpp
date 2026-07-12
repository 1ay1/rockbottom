// widgets/verdict.hpp — the headline banner: bottom's reason to exist.
//
// One glance answers "what is going on?". The banner frame, glyph and
// headline all carry the health color; the culprit detail sits dimmer
// beside it. A right chip shows the three load averages as a trend arrow.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"

#include <algorithm>
#include <string>

namespace rockbottom::ui {

class VerdictBanner {
    const Snapshot& snap_;
    int pulse_ = 0;   // >0 → health just degraded; flare the frame

public:
    explicit VerdictBanner(const Snapshot& s, int pulse = 0)
        : snap_(s), pulse_(std::clamp(pulse, 0, 3)) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        const Verdict& v = snap_.verdict;
        Color c = health_color(v.level);
        // Pulse: on a health degrade the border flares toward white, then
        // fades back over the next ticks (3→2→1→0).
        Color frame = pulse_ > 0 ? mix(c, pal::white, 0.25 * pulse_) : c;

        // load trend: comparing 1m to 15m tells you if things are heating up.
        const auto& la = snap_.cpu.loadavg;
        const char* trend = la[0] > la[2] * 1.3 ? "↗" : la[0] < la[2] * 0.7 ? "↘" : "→";
        std::string chip = "load " + fmt::fixed2(la[0]) + " " + trend;

        // PSI stall chips appear only when a resource is actually contended —
        // silence is a feature.
        const Psi& psi = snap_.psi;
        auto add_psi = [&](const char* tag, const PsiEntry& e) {
            if (e.available && e.some_avg10 >= 5)
                chip += " · " + std::string(tag) + " stall " +
                        std::to_string(static_cast<int>(e.some_avg10)) + "%";
        };
        add_psi("cpu", psi.cpu);
        add_psi("mem", psi.mem);
        add_psi("io",  psi.io);

        auto b = vstack();
        b.border(BorderStyle::Round)
         .border_color(frame)
         .border_text_end(" " + chip + " ", BorderTextPos::Top)
         .padding(0, 1, 0, 1);
        if (theme_paints_canvas())
            b.bg(theme_canvas());   // frame reads on the theme bg, not default

        // One TextElement + StyledRuns: flex can never shrink the headline —
        // overflow truncates the tail of the detail instead (TruncateEnd).
        // Leads with an inverted-color PILL (" ● CALM ") — the single anchor
        // your eye lands on first, readable from across the room.
        std::string content;
        std::vector<StyledRun> runs;
        auto add = [&](const std::string& txt, Style st) {
            if (txt.empty()) return;
            std::size_t off = content.size();
            content += txt;
            runs.push_back({off, txt.size(), st});
        };
        add(std::string(" ") + health_glyph(v.level) + " " + health_word(v.level) + " ",
            Style{}.with_bold().with_fg(pal::bg).with_bg(c));
        add("  ", Style{});
        add(v.headline, Style{}.with_bold().with_fg(c));
        add("  ·  ", Style{}.with_fg(pal::faint));
        add(v.detail, Style{}.with_fg(pal::label));

        std::vector<Element> line;
        line.push_back(Element{TextElement{
            .content = std::move(content),
            .style   = {},
            .wrap    = TextWrap::TruncateEnd,
            .runs    = std::move(runs),
        }});
        // Pin to exactly 3 rows (top border + headline + bottom border). On a
        // short terminal the vstack's grow child (the proc table) would
        // otherwise flex-shrink the banner, clipping its single content row
        // and leaving an empty ╭──╮/╰──╯ shell. A fixed height makes the
        // headline non-negotiable — it's the whole reason the tool exists.
        return (b(std::move(line)) | height(3)).build();
    }
};

}  // namespace rockbottom::ui
