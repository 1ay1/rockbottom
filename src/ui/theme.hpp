// theme.hpp — bottom respects the TERMINAL's own theme. Every color is a
// native ANSI-16 slot (Color::red()/green()/…), so the palette is whatever
// the user's terminal defines — Gruvbox, Catppuccin, Solarized, Nord, your
// grandma's amber CRT. We paint semantics (good/warn/crit, per-domain
// accents), the terminal paints the actual hues. Backgrounds default to the
// terminal's own bg (default_color) — we never force a canvas color.

#pragma once

#include <maya/maya.hpp>

#include "../core/metrics.hpp"

#include <algorithm>

namespace rockbottom::ui {

namespace pal {
// Surfaces — let the terminal own the canvas; structure reads via the
// dim/bright greys the user's theme already defines.
inline constexpr auto bg       = maya::Color::default_color();
inline constexpr auto bg_panel = maya::Color::default_color();
inline constexpr auto border   = maya::Color::bright_black();
inline constexpr auto track    = maya::Color::bright_black();   // meter groove
inline constexpr auto rail     = maya::Color::bright_black();   // table header band
inline constexpr auto sel_bg   = maya::Color::bright_black();   // selected-row strip

// Ink tiers — the terminal's fg + its two grey slots.
inline constexpr auto white    = maya::Color::bright_white();
inline constexpr auto text     = maya::Color::white();         // normal fg
inline constexpr auto label    = maya::Color::white();
inline constexpr auto dim      = maya::Color::bright_black();
inline constexpr auto faint    = maya::Color::bright_black();

// Semantic hues — the terminal decides what "green" looks like.
inline constexpr auto good     = maya::Color::green();
inline constexpr auto warn     = maya::Color::yellow();
inline constexpr auto hot      = maya::Color::bright_yellow();
inline constexpr auto crit     = maya::Color::red();
inline constexpr auto blue     = maya::Color::blue();
inline constexpr auto mauve    = maya::Color::magenta();
inline constexpr auto teal     = maya::Color::cyan();
inline constexpr auto sky      = maya::Color::bright_cyan();
inline constexpr auto pink     = maya::Color::bright_magenta();

// Per-domain signature accents.
inline constexpr auto cpu_ac  = blue;
inline constexpr auto mem_ac  = mauve;
inline constexpr auto disk_ac = teal;
inline constexpr auto net_ac  = good;
inline constexpr auto proc_ac = hot;
}  // namespace pal

// With native ANSI colors there's no RGB to interpolate — a "blend" just
// picks one of the two endpoints by which side of the midpoint t falls on.
// Structure code that used mix() for subtle tints degrades to the base color.
[[nodiscard]] inline maya::Color mix(maya::Color a, maya::Color b, double t) {
    return t < 0.5 ? a : b;
}

// The ANSI-16 way to "lift toward white": promote a normal slot to its
// bright counterpart (green → bright green). This is what selection ink,
// glossy meter tips and hot rails use now that mix() can't interpolate —
// the terminal's own bright variant IS the theme-correct highlight.
// bright_black (the dim/faint ink) has no brighter grey to go to; on a
// selection strip that's ALSO bright_black it would disappear, so it lifts
// to white instead.
[[nodiscard]] inline maya::Color brighten(maya::Color c) {
    if (c.kind() == maya::Color::Kind::Named) {
        if (c.index() < 8)
            return maya::Color{static_cast<maya::AnsiColor>(c.index() + 8)};
        if (c.index() == 8)   // bright_black → white: visible on the strip
            return maya::Color::white();
    }
    return c;
}

// Load ramp, snapped to the terminal's own green/yellow/red. No gradient —
// the terminal owns the hues, so we step through its semantic slots.
[[nodiscard]] inline maya::Color load_color(double f) {
    f = std::clamp(f, 0.0, 1.0);
    if (f < 0.55) return pal::good;   // green
    if (f < 0.80) return pal::warn;   // yellow
    if (f < 0.92) return pal::hot;    // bright yellow / orange
    return pal::crit;                 // red
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

}  // namespace rockbottom::ui
