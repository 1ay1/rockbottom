// theme.hpp — bottom's visual identity: Catppuccin-Mocha-inspired palette,
// semantic load coloring, and per-domain accent hues so every panel is
// instantly identifiable.

#pragma once

#include <maya/maya.hpp>

#include "../core/metrics.hpp"

#include <algorithm>

namespace bottom::ui {

namespace pal {
inline constexpr auto bg       = maya::Color::hex(0x11111b);
inline constexpr auto bg_panel = maya::Color::hex(0x181825);
inline constexpr auto border   = maya::Color::hex(0x313244);
inline constexpr auto track    = maya::Color::hex(0x26263a);   // meter groove
inline constexpr auto label    = maya::Color::hex(0x9399b2);
inline constexpr auto dim      = maya::Color::hex(0x6c7086);
inline constexpr auto faint    = maya::Color::hex(0x45475a);
inline constexpr auto text     = maya::Color::hex(0xcdd6f4);
inline constexpr auto white    = maya::Color::hex(0xf5f5ff);
inline constexpr auto good     = maya::Color::hex(0xa6e3a1);   // green
inline constexpr auto warn     = maya::Color::hex(0xf9e2af);   // yellow
inline constexpr auto hot      = maya::Color::hex(0xfab387);   // orange
inline constexpr auto crit     = maya::Color::hex(0xf38ba8);   // red
inline constexpr auto blue     = maya::Color::hex(0x89b4fa);
inline constexpr auto mauve    = maya::Color::hex(0xcba6f7);
inline constexpr auto teal     = maya::Color::hex(0x94e2d5);
inline constexpr auto sky      = maya::Color::hex(0x89dceb);
inline constexpr auto pink     = maya::Color::hex(0xf5c2e7);

// Per-domain signature accents.
inline constexpr auto cpu_ac  = blue;
inline constexpr auto mem_ac  = mauve;
inline constexpr auto disk_ac = teal;
inline constexpr auto net_ac  = good;
inline constexpr auto proc_ac = hot;
}  // namespace pal

// Linear RGB blend, t ∈ [0,1].
[[nodiscard]] inline maya::Color mix(maya::Color a, maya::Color b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto l = [&](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(static_cast<double>(x) +
                                    (static_cast<double>(y) - static_cast<double>(x)) * t);
    };
    return maya::Color::rgb(l(a.r(), b.r()), l(a.g(), b.g()), l(a.b(), b.b()));
}

// Smooth green→yellow→orange→red for a load fraction. Used for *values*.
[[nodiscard]] inline maya::Color load_color(double f) {
    f = std::clamp(f, 0.0, 1.0);
    if (f < 0.45) return pal::good;
    if (f < 0.70) return mix(pal::good, pal::warn, (f - 0.45) / 0.25);
    if (f < 0.90) return mix(pal::warn, pal::hot,  (f - 0.70) / 0.20);
    return mix(pal::hot, pal::crit, (f - 0.90) / 0.10);
}

[[nodiscard]] inline maya::Color health_color(Health h) {
    switch (h) {
        case Health::Calm:     return pal::good;
        case Health::Busy:     return pal::blue;
        case Health::Stressed: return pal::hot;
        case Health::Critical: return pal::crit;
    }
    return pal::good;
}

[[nodiscard]] inline const char* health_glyph(Health h) {
    switch (h) {
        case Health::Calm:     return "●";
        case Health::Busy:     return "◆";
        case Health::Stressed: return "▲";
        case Health::Critical: return "✖";
    }
    return "●";
}

[[nodiscard]] inline const char* health_word(Health h) {
    switch (h) {
        case Health::Calm:     return "CALM";
        case Health::Busy:     return "BUSY";
        case Health::Stressed: return "STRESSED";
        case Health::Critical: return "CRITICAL";
    }
    return "CALM";
}

}  // namespace bottom::ui
