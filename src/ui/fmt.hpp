// fmt.hpp — tiny formatting helpers shared by the panel widgets.

#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace bottom::ui::fmt {

[[nodiscard]] inline std::string pct(double frac) {
    char b[8];
    std::snprintf(b, sizeof b, "%d%%", static_cast<int>(std::lround(frac * 100)));
    return b;
}

// Right-aligned 4-char percent ("  7%", " 38%", "100%") — numbers form a
// clean scannable column instead of ragging left.
[[nodiscard]] inline std::string pct_pad(double frac) {
    char b[8];
    std::snprintf(b, sizeof b, "%3d%%", static_cast<int>(std::lround(frac * 100)));
    return b;
}

[[nodiscard]] inline std::string fixed1(double v) {
    char b[16];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

[[nodiscard]] inline std::string fixed2(double v) {
    char b[16];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

// Truncate a UTF-8-unaware ASCII-ish name to n display cells with ellipsis.
[[nodiscard]] inline std::string clip(const std::string& s, std::size_t n) {
    if (s.size() <= n) return s;
    return s.substr(0, n - 1) + "…";
}

// "AMD Ryzen 5 1600 Six-Core Processor" → "AMD Ryzen 5 1600 Six-Core"
[[nodiscard]] inline std::string short_model(std::string s) {
    for (const char* junk : {"(R)", "(TM)", "CPU", "Processor", "  "}) {
        std::size_t p;
        while ((p = s.find(junk)) != std::string::npos) s.erase(p, std::strlen(junk));
    }
    auto a = s.find_first_not_of(' ');
    if (a != std::string::npos) s = s.substr(a);
    auto b = s.find_last_not_of(' ');
    if (b != std::string::npos) s = s.substr(0, b + 1);
    if (s.size() > 30) s = s.substr(0, 29) + "…";
    return s.empty() ? "CPU" : s;
}

}  // namespace bottom::ui::fmt
