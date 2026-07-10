// widgets/spark.hpp — bottom::ui::Spark, a value-colored history sparkline.
//
// Same idiom as maya::Sparkline (one TextElement + StyledRun spans) but each
// column is colored by its own value through the load gradient, so a spike
// glows orange/red in the history while calm periods stay green/dim.
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

    Spark& cells(int n)             { cells_ = std::max(1, n); return *this; }
    Spark& color(maya::Color c)     { color_ = c; return *this; }
    Spark& dim_low(bool b)          { dim_low_ = b; return *this; }

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        static constexpr const char* kBars[] =
            {"⡀", "⣀", "⣄", "⣤", "⣦", "⣶", "⣷", "⣿"};
        static constexpr const char* kEmpty = "⠄";   // faint baseline dot

        std::string content;
        std::vector<maya::StyledRun> runs;
        content.reserve(static_cast<std::size_t>(cells_) * 3);

        auto push = [&](const char* glyph, maya::Color c) {
            std::size_t off = content.size();
            content += glyph;
            runs.push_back({off, content.size() - off, maya::Style{}.with_fg(c)});
        };

        const int start = len_ > cells_ ? len_ - cells_ : 0;
        const int shown = len_ - start;

        // Left-pad with baseline dots until history fills the window.
        for (int i = shown; i < cells_; ++i) push(kEmpty, pal::faint);

        for (int i = start; i < len_; ++i) {
            float v = std::clamp(data_[i], 0.0f, 1.0f);
            int idx = std::clamp(static_cast<int>(v * 7.0f + 0.5f), 0, 7);
            maya::Color c = color_ ? *color_ : load_color(v);
            if (dim_low_ && v < 0.02f) { c = pal::faint; idx = 0; }
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
