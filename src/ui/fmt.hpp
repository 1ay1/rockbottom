// fmt.hpp — tiny formatting helpers shared by the panel widgets.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace rockbottom::ui::fmt {

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

// Truncate a UTF-8 string to at most n display cells, with a trailing ellipsis
// when it doesn't fit. Counts and cuts on whole codepoints (not bytes), so a
// name containing accented Latin, CJK or emoji is never split mid-sequence into
// a broken glyph. (Treats each codepoint as one cell — good enough for the
// single-width scripts in process/interface names; double-width CJK will read a
// touch wide but never corrupt.) n==0 yields the empty string.
[[nodiscard]] inline std::string clip(const std::string& s, std::size_t n) {
    // Count codepoints; bail early the moment we know it fits in n cells.
    std::size_t cells = 0;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (++cells > n) break;
    }
    if (cells <= n) return s;
    if (n == 0) return {};
    // Keep n-1 codepoints, then the ellipsis (which is itself one cell).
    std::size_t i = 0, kept = 0;
    while (i < s.size() && kept < n - 1) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        ++kept;
    }
    return s.substr(0, i) + "…";
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

// Compact human count: 950 → "950", 12345 → "12.3K", 4.2e6 → "4.2M".
[[nodiscard]] inline std::string count(double n) {
    char b[24];
    if (n < 1000)       std::snprintf(b, sizeof b, "%d", static_cast<int>(n + 0.5));
    else if (n < 1e6)   std::snprintf(b, sizeof b, "%.1fK", n / 1e3);
    else if (n < 1e9)   std::snprintf(b, sizeof b, "%.1fM", n / 1e6);
    else                std::snprintf(b, sizeof b, "%.1fB", n / 1e9);
    return b;
}

// Compact age from seconds: "14s", "3m 09s", "2h 41m", "6d 4h".
[[nodiscard]] inline std::string age(std::uint64_t sec) {
    char b[32];
    if (sec < 60)          std::snprintf(b, sizeof b, "%llus", (unsigned long long)sec);
    else if (sec < 3600)   std::snprintf(b, sizeof b, "%llum %02llus", (unsigned long long)(sec / 60), (unsigned long long)(sec % 60));
    else if (sec < 86400)  std::snprintf(b, sizeof b, "%lluh %02llum", (unsigned long long)(sec / 3600), (unsigned long long)(sec % 3600 / 60));
    else                   std::snprintf(b, sizeof b, "%llud %lluh", (unsigned long long)(sec / 86400), (unsigned long long)(sec % 86400 / 3600));
    return b;
}

// Cumulative CPU time from milliseconds: "41ms", "3.2s", "4m 07s", "2h 13m".
[[nodiscard]] inline std::string cpu_time(std::uint64_t ms) {
    char b[32];
    if (ms < 1000)            std::snprintf(b, sizeof b, "%llums", (unsigned long long)ms);
    else if (ms < 60'000)     std::snprintf(b, sizeof b, "%.1fs", static_cast<double>(ms) / 1000.0);
    else if (ms < 3'600'000)  std::snprintf(b, sizeof b, "%llum %02llus", (unsigned long long)(ms / 60000), (unsigned long long)(ms % 60000 / 1000));
    else                      std::snprintf(b, sizeof b, "%lluh %02llum", (unsigned long long)(ms / 3600000), (unsigned long long)(ms % 3600000 / 60000));
    return b;
}

}  // namespace rockbottom::ui::fmt
