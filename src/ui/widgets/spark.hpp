// widgets/spark.hpp — bottom::ui::Spark, a value-colored history sparkline.
//
// Same idiom as maya::Sparkline (one TextElement + StyledRun spans) but each
// column is colored by its own value through the load gradient, so a spike
// glows orange/red in the history while calm periods stay green/dim.
//
// DESIGN RULE: silence is blank. Near-zero history renders as SPACE, not
// baseline dots — a flat-lined spark should disappear entirely so the eye
// is drawn only to actual activity. If the whole window is quiet the widget
// paints nothing but empty cells.
//
//   Spark{hist.data(), len}.cells(14)                 → load-gradient columns
//   Spark{rx.data(), len}.cells(14).color(pal::net_ac) → flat accent columns

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace bottom::ui {

class Spark {
    const float* data_ = nullptr;
    int len_ = 0;
    int cells_ = 12;
    std::optional<maya::Color> color_;   // nullopt → per-value load gradient
    bool dim_low_ = true;                // fade near-zero columns into the bg

public:
    Spark(const float* data, int len) : data_(data), len_(std::max(0, len)) {}

    Spark& cells(int n)             { cells_ = n; return *this; }   // <=0 → fill
    Spark& color(maya::Color c)     { color_ = c; return *this; }
    Spark& dim_low(bool b)          { dim_low_ = b; return *this; }
    Spark& fill()                   { cells_ = 0; return *this; }

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        if (cells_ <= 0) {
            Spark self = *this;
            return maya::Element{maya::ComponentElement{
                .render = [self](int w, int) -> maya::Element {
                    Spark s = self;
                    s.cells_ = std::max(1, w);
                    return s.build_fixed();
                },
            }};
        }
        return build_fixed();
    }

    [[nodiscard]] maya::Element build_fixed() const {
        static constexpr const char* kBars[] =
            {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

        std::string content;
        std::vector<maya::StyledRun> runs;
        content.reserve(static_cast<std::size_t>(cells_) * 3);

        auto push = [&](const char* glyph, maya::Color c) {
            std::size_t off = content.size();
            content += glyph;
            runs.push_back({off, content.size() - off, maya::Style{}.with_fg(c)});
        };
        auto push_blank = [&] {
            std::size_t off = content.size();
            content += ' ';
            runs.push_back({off, 1, maya::Style{}});
        };

        const int start = len_ > cells_ ? len_ - cells_ : 0;
        const int shown = len_ - start;

        // Left-pad with true blanks until history fills the window.
        for (int i = shown; i < cells_; ++i) push_blank();

        for (int i = start; i < len_; ++i) {
            float v = std::clamp(data_[i], 0.0f, 1.0f);
            // Silence rule: a near-zero sample is a SPACE. Only activity inks.
            if (dim_low_ && v < 0.03f) { push_blank(); continue; }
            int idx = std::clamp(static_cast<int>(v * 7.0f + 0.5f), 0, 7);
            maya::Color c = color_ ? *color_ : load_color(v);
            push(kBars[idx], c);
        }

        return maya::Element{maya::TextElement{
            .content = std::move(content),
            .style   = {},
            .wrap    = maya::TextWrap::NoWrap,
            .runs    = std::move(runs),
        }};
    }
};

}  // namespace bottom::ui
