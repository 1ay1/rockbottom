// widgets/logo.hpp — the rockbottom wordmark, as an ASCII-art splash.
//
// A block-letter "ROCKBOTTOM" under the brand's mauve→blue gradient, with the
// signature ▁▂▃▄▅▆▇ mountain sliding down to a flat "rock bottom" line and the
// one-sentence pitch. Used at the top of the help overlay (the closest thing
// the app has to an about screen). Purely decorative — it measures its own
// natural size and gradient-tints per column so it tracks any theme.

#pragma once

#include <maya/maya.hpp>
#include <maya/widget/gradient.hpp>

#include "../theme.hpp"

#include <array>
#include <string>
#include <vector>

namespace rockbottom::ui {

class Logo {
    bool compact_ = false;   // narrow terminals get the small mark

public:
    explicit Logo(bool compact = false) : compact_(compact) {}

    operator maya::Element() const { return build(); }

    // Widest line of the full logo — callers use it to decide compact vs full.
    static constexpr int kFullWidth = 58;

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        // Each glyph line is coloured left→right along the mauve→blue ramp so
        // the whole wordmark reads as one gradient block (like the header).
        auto grad_line = [](const std::string& s) -> Element {
            return gradient(s, pal::mauve, pal::blue, /*bold=*/true);
        };

        if (compact_) {
            // Tiny mark: a mountain that bottoms out, then the name + pitch.
            return (v(
                grad_line("▁▂▃▄▅▆▇█▇▆▅▄▃▂▁▁▁▁▁▁▁"),
                grad_line("rockbottom"),
                text("your computer is fine, buddy.") | nowrap | fgc(pal::label)
            )).build();
        }

        // Full block wordmark. A hand-set 3-row slab of the ten letters.
        static const std::array<const char*, 3> art = {
            "█▀▄ █▀█ █▀▀ █ █ █▀▄ █▀█ ▀█▀ ▀█▀ █▀█ █▄█",
            "█▀▄ █ █ █   █▀▄ █▀▄ █ █  █   █   █ █ █ █",
            "▀ ▀ ▀▀▀ ▀▀▀ ▀ ▀ ▀▀▀ ▀▀▀  ▀   ▀   ▀▀▀ ▀ ▀",
        };

        std::vector<Element> rows;
        for (const char* line : art) rows.push_back(grad_line(line));

        // The mountain falls to a flat line — the machine hitting rock bottom —
        // then the pitch, in the quiet inks.
        rows.push_back((h(
            grad_line("  ▇▆▅▄▃▂▁▁▁▁▁▁▁▁▁▁▁▁  "),
            text("your computer is fine, buddy.",
                 Style{}.with_fg(pal::label).with_bold()) | nowrap
        )).build());
        rows.push_back((h(
            text("  the one system monitor that ", Style{}.with_fg(pal::dim)) | nowrap,
            text("reads the scary numbers ", Style{}.with_fg(pal::label)) | nowrap,
            text("for", Style{}.with_fg(pal::sky).with_bold()) | nowrap,
            text(" you.", Style{}.with_fg(pal::label)) | nowrap
        )).build());

        return (v(std::move(rows))).build();
    }
};

}  // namespace rockbottom::ui
