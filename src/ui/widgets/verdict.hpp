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

namespace bottom::ui {

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

        std::vector<Element> line;
        line.push_back((h(
            text(std::string(health_glyph(v.level)) + " ") | nowrap | Bold | fgc(c),
            text(v.headline) | nowrap | Bold | fgc(c),
            text("  ·  ") | nowrap | fgc(pal::faint),
            text(v.detail) | nowrap | fgc(pal::label),
            space
        )).build());
        return b(std::move(line));
    }
};

}  // namespace bottom::ui
