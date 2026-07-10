// widgets/graph.hpp — bottom::ui::Graph, a multi-row filled area chart.
//
// The big brother of Spark: renders a history ring as an N-row filled
// mountain using vertical eighth-blocks (▁▂▃▄▅▆▇█) for sub-row resolution,
// one StyledRun per column per row. Columns are load-gradient colored by
// their own value, so peaks glow amber/red while calm stretches stay green.
//
//   Graph{hist.data(), len}.cells(40).rows(4)
//
// Same silence rule as Spark: near-zero columns paint nothing.

#pragma once

#include <maya/maya.hpp>

#include "../theme.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace bottom::ui {

class Graph {
    const float* data_ = nullptr;
    int len_ = 0;
    int cells_ = 40;
    int rows_ = 4;
    std::optional<maya::Color> color_;   // nullopt → per-value load gradient

public:
    Graph(const float* data, int len) : data_(data), len_(std::max(0, len)) {}

    Graph& cells(int n)         { cells_ = std::max(1, n); return *this; }
    Graph& rows(int n)          { rows_ = std::max(1, n); return *this; }
    Graph& color(maya::Color c) { color_ = c; return *this; }

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        static constexpr const char* kBlocks[] =
            {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

        const int start = len_ > cells_ ? len_ - cells_ : 0;
        const int pad = cells_ - (len_ - start);

        std::vector<Element> lines;
        lines.reserve(static_cast<std::size_t>(rows_));

        // Rows render top-down. A column with value v fills v*rows_ rows from
        // the bottom; the boundary row gets an eighth-block partial.
        for (int r = 0; r < rows_; ++r) {
            const double row_top    = static_cast<double>(rows_ - r) / rows_;
            const double row_bottom = static_cast<double>(rows_ - r - 1) / rows_;

            std::string content;
            std::vector<StyledRun> runs;
            content.reserve(static_cast<std::size_t>(cells_) * 3);

            auto put = [&](const std::string& glyph, Style st) {
                std::size_t off = content.size();
                content += glyph;
                runs.push_back({off, content.size() - off, st});
            };

            for (int i = 0; i < pad; ++i) put(" ", Style{});
            for (int i = start; i < len_; ++i) {
                const double v = std::clamp(static_cast<double>(data_[i]), 0.0, 1.0);
                if (v <= row_bottom + 0.002) { put(" ", Style{}); continue; }
                Color c = color_ ? *color_ : load_color(v);
                if (v >= row_top) { put("█", Style{}.with_fg(c)); continue; }
                int idx = std::clamp(
                    static_cast<int>((v - row_bottom) * rows_ * 8.0), 1, 8);
                put(kBlocks[idx - 1], Style{}.with_fg(c));
            }

            lines.push_back(Element{TextElement{
                .content = std::move(content),
                .style   = {},
                .wrap    = TextWrap::NoWrap,
                .runs    = std::move(runs),
            }});
        }

        return maya::dsl::v(lines).build();
    }
};

}  // namespace bottom::ui
