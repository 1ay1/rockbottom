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

#include <string>

namespace rockbottom::ui {

class VerdictBanner {
    const Snapshot& snap_;

public:
    explicit VerdictBanner(const Snapshot& s) : snap_(s) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        const Verdict& v = snap_.verdict;
        Color c = health_color(v.level);

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
         .border_color(c)
         .border_text_end(" " + chip + " ", BorderTextPos::Top)
         .padding(0, 1, 0, 1);

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
        return b(std::move(line));
    }
};

}  // namespace rockbottom::ui
