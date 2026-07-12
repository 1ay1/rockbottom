// theme.hpp — rockbottom's palette engine.
//
// HISTORY: bottom used to defer entirely to the terminal's own ANSI-16 theme.
// That is still available as the "native" theme (every slot is a Color::*()
// named ANSI slot, so it renders in the user's own Gruvbox/Nord/whatever). But
// maya::Color is a full truecolor value type that auto-degrades to 256- and
// 16-color on terminals without 24-bit support, so we ALSO ship a big deck of
// hand-tuned RGB themes you can cycle live with `T` (persisted between runs).
//
// DESIGN: a theme is authored as a compact `Spec` — a background, a foreground,
// the four-rung load ramp (good→warn→hot→crit) and six spectral accents. From
// those, build_theme() DERIVES the surface tiers (panel bg, borders, groove,
// selection strip) and the ink tiers (white/text/label/dim/faint) with
// consistent contrast math, so every theme in the deck has structurally
// identical legibility even though the hues differ wildly. Per-domain accents
// (cpu/mem/disk/net/gpu/proc) default to spectral picks but can be overridden.
//
// The rest of the codebase reads the palette through `pal::dim`, `pal::good`,
// `pal::cpu_ac`, … — those are inline REFERENCES into a single mutable
// `g_active` object, so switching a theme just copies new values into
// g_active's fields and all ~135 call sites pick up the change next frame.
//
// CONTRAST: every accent was picked to stand off the theme's own dark canvas;
// the four ramp rungs stay ordinally distinct (a warn you can tell from a hot);
// `dim`/`faint` read as structure without competing with live data; and
// `text`/`white` stay high-luma against bg.

#pragma once

#include <maya/maya.hpp>

#include "../core/metrics.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rockbottom::ui {

// ── the semantic palette ────────────────────────────────────────────────────
// Every field is a maya::Color. RGB themes fill these with truecolor; the
// "native" theme fills them with ANSI-16 named slots so the terminal owns the
// hues. maya degrades RGB → 256 → 16 automatically on lesser terminals.
struct Theme {
    const char* name;

    // Surfaces / structure.
    maya::Color bg, bg_panel, border, track, rail, sel_bg;
    // Ink tiers.
    maya::Color white, text, label, dim, faint;
    // Semantic ramp + spectral accents.
    maya::Color good, warn, hot, crit;
    maya::Color blue, mauve, teal, sky, pink, amber;
    // Per-domain signature accents.
    maya::Color cpu_ac, mem_ac, disk_ac, net_ac, gpu_ac, proc_ac;
};

namespace detail {
using C = maya::Color;

// ── RGB helpers (host-side, so they can use float math freely) ───────────────
struct Rgb { double r, g, b; };  // channels in 0..255

[[nodiscard]] inline Rgb to_rgb(std::uint32_t hex) {
    return {static_cast<double>((hex >> 16) & 0xFF),
            static_cast<double>((hex >> 8) & 0xFF),
            static_cast<double>(hex & 0xFF)};
}
[[nodiscard]] inline C from_rgb(Rgb c) {
    auto q = [](double v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0, 255.0) + 0.5); };
    return C::rgb(q(c.r), q(c.g), q(c.b));
}
// Linear blend a→b by t (0..1).
[[nodiscard]] inline Rgb lerp(Rgb a, Rgb b, double t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
// Perceived luma (Rec.601), 0..255.
[[nodiscard]] inline double luma(Rgb c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; }

// A theme is AUTHORED as this compact spec. Structure + ink tiers are derived.
struct Spec {
    const char* name;
    std::uint32_t bg;    // canvas base (the darkest surface)
    std::uint32_t fg;    // primary ink
    std::uint32_t good, warn, hot, crit;             // 4-rung load ramp
    std::uint32_t blue, mauve, teal, sky, pink, amber;  // spectral accents
    // Per-domain accents. 0xFFFFFFFF = "derive from spectral defaults".
    std::uint32_t cpu = 0xFFFFFFFF, mem = 0xFFFFFFFF, disk = 0xFFFFFFFF,
                  net = 0xFFFFFFFF, gpu = 0xFFFFFFFF, proc = 0xFFFFFFFF;
};

// Sentinel meaning "derive this domain accent from the spectral defaults".
inline constexpr std::uint32_t kDerive = 0xFFFFFFFF;

// Factory: the 12 core hues are required; the six domain accents default to the
// derive sentinel. Trailing defaults let a theme override only what it wants
// (mk(..., /*net=*/0xa6e3a1)) without listing every field — and, unlike a mixed
// designated aggregate, this is legal C++.
[[nodiscard]] inline Spec mk(
    const char* name, std::uint32_t bg, std::uint32_t fg,
    std::uint32_t good, std::uint32_t warn, std::uint32_t hot, std::uint32_t crit,
    std::uint32_t blue, std::uint32_t mauve, std::uint32_t teal,
    std::uint32_t sky, std::uint32_t pink, std::uint32_t amber,
    std::uint32_t net = kDerive, std::uint32_t gpu = kDerive,
    std::uint32_t proc = kDerive, std::uint32_t cpu = kDerive,
    std::uint32_t mem = kDerive, std::uint32_t disk = kDerive) {
    return Spec{name, bg, fg, good, warn, hot, crit,
                blue, mauve, teal, sky, pink, amber,
                cpu, mem, disk, net, gpu, proc};
}

// Derive a full Theme from a Spec with consistent contrast math. `bg` is the
// canvas; surfaces step UP from it toward the fg; ink tiers step DOWN from fg
// toward bg. The exact ratios were tuned once so every theme in the deck reads
// with identical structural legibility.
[[nodiscard]] inline Theme build_theme(const Spec& s) {
    const Rgb bg = to_rgb(s.bg);
    const Rgb fg = to_rgb(s.fg);
    // Lift bg toward fg for surface tiers (panels/borders sit above the canvas).
    auto up   = [&](double t) { return from_rgb(lerp(bg, fg, t)); };
    // Pull fg toward bg for ink tiers (dimmer text recedes into the canvas).
    auto down = [&](double t) { return from_rgb(lerp(fg, bg, t)); };

    auto pick = [](std::uint32_t v, std::uint32_t fallback) {
        return from_rgb(to_rgb(v == kDerive ? fallback : v));
    };

    Theme t{};
    t.name = s.name;

    // Surfaces — canvas, then progressively lighter structure.
    // `bg` is ALSO the punch-out badge ink (fgc(pal::bg) over a bright chip),
    // so it must read dark against the accent chips. On a LIGHT theme the
    // canvas is bright, so a bright `bg` would make badge text invisible; keep
    // badge ink dark there while `bg_panel` stays the true (light) canvas.
    const bool light = luma(bg) > 140.0;
    t.bg       = light ? from_rgb(lerp(bg, {0, 0, 0}, 0.82)) : from_rgb(bg);
    t.bg_panel = from_rgb(bg);        // the real canvas (painted by the app)
    t.border   = up(0.22);            // panel outline: clearly above the canvas
    t.track    = up(0.14);            // meter groove: subtle
    t.rail     = up(0.16);            // table header band
    t.sel_bg   = up(0.20);            // selected-row strip

    // Ink tiers — bright headline down to faint structure. On a light theme
    // "brighten toward white" would REDUCE contrast, so the headline moves
    // toward black instead; dim/faint recede toward the (light) canvas.
    t.white = light ? from_rgb(lerp(fg, {0, 0, 0}, 0.25))
                    : from_rgb(lerp(fg, {255, 255, 255}, 0.35));
    t.text  = from_rgb(fg);
    t.label = down(0.14);
    t.dim   = down(0.52);             // structure ink — legible but recessive
    t.faint = down(0.66);             // faintest guide lines

    // Semantic ramp.
    t.good = from_rgb(to_rgb(s.good));
    t.warn = from_rgb(to_rgb(s.warn));
    t.hot  = from_rgb(to_rgb(s.hot));
    t.crit = from_rgb(to_rgb(s.crit));

    // Spectral accents.
    t.blue  = from_rgb(to_rgb(s.blue));
    t.mauve = from_rgb(to_rgb(s.mauve));
    t.teal  = from_rgb(to_rgb(s.teal));
    t.sky   = from_rgb(to_rgb(s.sky));
    t.pink  = from_rgb(to_rgb(s.pink));
    t.amber = from_rgb(to_rgb(s.amber));

    // Domain accents — default to spectral picks that stay mutually distinct:
    // cpu blue · mem mauve · disk teal · net good/green · gpu sky · proc pink.
    t.cpu_ac  = pick(s.cpu,  s.blue);
    t.mem_ac  = pick(s.mem,  s.mauve);
    t.disk_ac = pick(s.disk, s.teal);
    t.net_ac  = pick(s.net,  s.good);
    t.gpu_ac  = pick(s.gpu,  s.sky);
    t.proc_ac = pick(s.proc, s.pink);
    return t;
}

// ── the native (ANSI) theme ──────────────────────────────────────────────────
// The one theme NOT derived from a Spec: every slot is a named ANSI-16 color so
// the terminal owns the hues, and bg stays default so we never paint the canvas.
[[nodiscard]] inline Theme native_theme() {
    return Theme{"native",
        C::default_color(), C::default_color(), C::bright_black(),
        C::bright_black(), C::bright_black(), C::bright_black(),
        C::bright_white(), C::white(), C::white(),
        C::bright_black(), C::bright_black(),
        C::green(), C::yellow(), C::bright_red(), C::red(),
        C::blue(), C::magenta(), C::cyan(), C::bright_cyan(),
        C::bright_magenta(), C::bright_yellow(),
        C::blue(), C::magenta(), C::cyan(), C::green(),
        C::bright_green(), C::bright_magenta()};
}

// ── the theme deck ──────────────────────────────────────────────────────────
// Index 0 is ALWAYS "native". Everything after is a Spec → build_theme(). Each
// Spec lists: name, bg, fg, then the load ramp (good/warn/hot/crit) and the six
// spectral accents (blue/mauve/teal/sky/pink/amber). Domain accents are derived
// unless a theme wants a specific one (trailing .net=…, etc.).
inline const std::vector<Spec>& theme_specs() {
    static const std::vector<Spec> s = {
        // ── Beloved editor palettes ──────────────────────────────────────────
        // catppuccin mocha — soft slate, lavender/blue/pink. The crowd favourite.
        mk("mocha", 0x1e1e2e, 0xcdd6f4,
           0xa6e3a1, 0xf9e2af, 0xfab387, 0xf38ba8,
           0x89b4fa, 0xcba6f7, 0x94e2d5, 0x89dceb, 0xf5c2e7, 0xfab387,
           /*net*/0xa6e3a1, /*gpu*/kDerive, /*proc*/0xf5c2e7),

        // catppuccin macchiato — a hair warmer/darker than mocha.
        mk("macchiato", 0x24273a, 0xcad3f5,
           0xa6da95, 0xeed49f, 0xf5a97f, 0xed8796,
           0x8aadf4, 0xc6a0f6, 0x8bd5ca, 0x91d7e3, 0xf5bde6, 0xf5a97f,
           0xa6da95, kDerive, 0xf5bde6),

        // catppuccin frappé — the mellow twilight variant.
        mk("frappe", 0x303446, 0xc6d0f5,
           0xa6d189, 0xe5c890, 0xef9f76, 0xe78284,
           0x8caaee, 0xca9ee6, 0x81c8be, 0x99d1db, 0xf4b8e4, 0xef9f76,
           0xa6d189, kDerive, 0xf4b8e4),

        // catppuccin latte — the LIGHT one. Proof the engine handles light bg.
        mk("latte", 0xeff1f5, 0x4c4f69,
           0x40a02b, 0xdf8e1d, 0xfe640b, 0xd20f39,
           0x1e66f5, 0x8839ef, 0x179299, 0x209fb5, 0xea76cb, 0xfe640b,
           0x40a02b, kDerive, 0xea76cb),

        // tokyo night — inky indigo, electric blue + magenta.
        mk("tokyo", 0x1a1b26, 0xc0caf5,
           0x9ece6a, 0xe0af68, 0xff9e64, 0xf7768e,
           0x7aa2f7, 0xbb9af7, 0x2ac3de, 0x7dcfff, 0xff75a0, 0xe0af68,
           0x9ece6a, /*gpu*/0x73daca),

        // tokyo storm — the softer, slightly lighter tokyo variant.
        mk("storm", 0x24283b, 0xc0caf5,
           0x9ece6a, 0xe0af68, 0xff9e64, 0xf7768e,
           0x7aa2f7, 0xbb9af7, 0x2ac3de, 0x7dcfff, 0xff75a0, 0xe0af68,
           0x9ece6a, 0x73daca),

        // gruvbox — warm retro amber/olive on brown-black. Cozy.
        mk("gruvbox", 0x1d2021, 0xebdbb2,
           0xb8bb26, 0xfabd2f, 0xfe8019, 0xfb4934,
           0x83a598, 0xd3869b, 0x8ec07c, 0x83a598, 0xd3869b, 0xfabd2f,
           0xb8bb26, kDerive, 0xfe8019),

        // gruvbox light — the daylight sibling. Warm parchment.
        mk("gruvbox-light", 0xfbf1c7, 0x3c3836,
           0x79740e, 0xb57614, 0xaf3a03, 0x9d0006,
           0x076678, 0x8f3f71, 0x427b58, 0x076678, 0x8f3f71, 0xb57614,
           0x79740e, kDerive, 0xaf3a03),

        // dracula — high-contrast purple/pink/green on graphite.
        mk("dracula", 0x282a36, 0xf8f8f2,
           0x50fa7b, 0xf1fa8c, 0xffb86c, 0xff5555,
           0xbd93f9, 0xff79c6, 0x8be9fd, 0x8be9fd, 0xff79c6, 0xffb86c,
           0x50fa7b, 0x9aedfe),

        // nord — cool arctic blues + frost. Calm and muted.
        mk("nord", 0x2e3440, 0xeceff4,
           0xa3be8c, 0xebcb8b, 0xd08770, 0xbf616a,
           0x81a1c1, 0xb48ead, 0x88c0d0, 0x8fbcbb, 0xb48ead, 0xebcb8b,
           0xa3be8c, 0x5e81ac),

        // solarized dark — the classic balanced-luma engineering palette.
        mk("solarized", 0x002b36, 0x93a1a1,
           0x859900, 0xb58900, 0xcb4b16, 0xdc322f,
           0x268bd2, 0x6c71c4, 0x2aa198, 0x2aa198, 0xd33682, 0xb58900,
           0x859900, kDerive, 0xd33682),

        // solarized light — the daytime engineering desk.
        mk("solarized-light", 0xfdf6e3, 0x586e75,
           0x859900, 0xb58900, 0xcb4b16, 0xdc322f,
           0x268bd2, 0x6c71c4, 0x2aa198, 0x2aa198, 0xd33682, 0xb58900,
           0x859900, kDerive, 0xd33682),

        // everforest — soft muted forest greens + warm sand. Easy on the eyes.
        mk("everforest", 0x2b3339, 0xd3c6aa,
           0xa7c080, 0xdbbc7f, 0xe69875, 0xe67e80,
           0x7fbbb3, 0xd699b6, 0x83c092, 0x7fbbb3, 0xd699b6, 0xdbbc7f,
           0xa7c080, kDerive, 0xe69875),

        // rose pine — dusky rose + iris + foam on plum ink.
        mk("rosepine", 0x191724, 0xe0def4,
           0x9ccfd8, 0xf6c177, 0xebbcba, 0xeb6f92,
           0x31748f, 0xc4a7e7, 0x9ccfd8, 0x9ccfd8, 0xebbcba, 0xf6c177,
           0x31748f, kDerive, 0xeb6f92),

        // rose pine moon — the moonlit variant, a touch brighter base.
        mk("rosepine-moon", 0x232136, 0xe0def4,
           0x9ccfd8, 0xf6c177, 0xea9a97, 0xeb6f92,
           0x3e8fb0, 0xc4a7e7, 0x9ccfd8, 0x9ccfd8, 0xea9a97, 0xf6c177,
           0x3e8fb0, kDerive, 0xeb6f92),

        // monokai — the vivid classic on warm charcoal.
        mk("monokai", 0x272822, 0xf8f8f2,
           0xa6e22e, 0xe6db74, 0xfd971f, 0xf92672,
           0x66d9ef, 0xae81ff, 0x66d9ef, 0xa1efe4, 0xf92672, 0xe6db74,
           0xa6e22e, kDerive, 0xfd971f),

        // ayu mirage — warm slate with orange + teal signals.
        mk("ayu", 0x1f2430, 0xcbccc6,
           0xbae67e, 0xffd580, 0xffa759, 0xf28779,
           0x73d0ff, 0xd4bfff, 0x95e6cb, 0x73d0ff, 0xf29e74, 0xffd580,
           0xbae67e, 0x5ccfe6),

        // kanagawa — Hokusai's wave: sumi-e ink, wave-crest blue, autumn.
        mk("kanagawa", 0x1f1f28, 0xdcd7ba,
           0x98bb6c, 0xe6c384, 0xffa066, 0xe82424,
           0x7e9cd8, 0x957fb8, 0x7aa89f, 0x7fb4ca, 0xd27e99, 0xe6c384,
           0x98bb6c, kDerive, 0xd27e99),

        // ── Neon / high-voltage ───────────────────────────────────────────────
        // synthwave — hot magenta + cyan on deep violet. Retro-future.
        mk("synthwave", 0x241b2f, 0xf8f8ff,
           0x72f1b8, 0xfede5d, 0xff8b39, 0xfe4450,
           0x36f9f6, 0xff7edb, 0x36f9f6, 0x72f1b8, 0xff7edb, 0xfede5d,
           0xb2ff66, 0x72f1b8, 0xff7edb),

        // cyberpunk — chartreuse + hot-pink on near-black, high voltage.
        mk("cyberpunk", 0x0d0d0f, 0xf0f0ff,
           0x00ff9f, 0xfcee0a, 0xff9f1c, 0xff003c,
           0x00b8ff, 0xd300ff, 0x00fff5, 0x00b8ff, 0xff00a0, 0xfcee0a,
           0xbdff00, 0x00ff9f, 0xff00a0),

        // outrun — sunset gradient neon: orange/pink/purple over midnight.
        mk("outrun", 0x150b28, 0xffe0f7,
           0x00e8c6, 0xffd319, 0xff8c00, 0xff2a6d,
           0x05d9e8, 0xd300ff, 0x00e8c6, 0x05d9e8, 0xff6ac1, 0xffd319,
           0x00e8c6, 0x05d9e8, 0xff6ac1),

        // vaporwave — pastel neon dream: mint, peach, lilac on twilight.
        mk("vaporwave", 0x1b1035, 0xf5e6ff,
           0x7cf9c0, 0xffe66d, 0xffa07a, 0xff6ec7,
           0x7ac7ff, 0xc792ea, 0x89ddff, 0x7ac7ff, 0xff9edb, 0xffe66d,
           0x7cf9c0, 0x89ddff, 0xff9edb),

        // hotline — Miami neon: pink & teal on black glass.
        mk("hotline", 0x11041c, 0xffe9f7,
           0x18ffd5, 0xffe14d, 0xff9d5c, 0xff1e56,
           0x2de2e6, 0xf706cf, 0x18ffd5, 0x2de2e6, 0xff2fb3, 0xffe14d,
           0x18ffd5, 0x2de2e6, 0xff2fb3),

        // ── Monochrome / retro CRT ─────────────────────────────────────────────
        // matrix — green phosphor CRT. Everything is a shade of green.
        mk("matrix", 0x001100, 0x66ff66,
           0x00ff41, 0xaaff00, 0xffcc00, 0xff3131,
           0x39ff14, 0x7fff55, 0x00e676, 0x66ff99, 0x9dff66, 0xaaff00,
           0x00ff41, 0x00e676, 0x9dff66),

        // amber CRT — monochrome amber terminal, warm nostalgia.
        mk("amber-crt", 0x1a0f00, 0xffb347,
           0xffcc33, 0xffb000, 0xff8800, 0xff5500,
           0xffc04d, 0xffa64d, 0xffd27f, 0xffe0a3, 0xff9933, 0xffb000,
           0xffcc33, 0xffd27f, 0xffe0a3),

        // ibm 3270 — cold green-white glass terminal.
        mk("3270", 0x0a0f0a, 0xd0f0d0,
           0x33ff88, 0xf5f56f, 0xffb347, 0xff5f5f,
           0x66d9ff, 0xb28dff, 0x66ffe0, 0x99e0ff, 0xff9de0, 0xf5f56f,
           0x33ff88, 0x66ffe0, 0xff9de0),

        // ── Ocean / nature moods ───────────────────────────────────────────────
        // oceanic — deep teal-blue sea with coral warnings.
        mk("oceanic", 0x0f1c26, 0xd8f0f5,
           0x5fd6a0, 0xf2d377, 0xf5a35c, 0xf26d6d,
           0x4dc4e0, 0x9d8cf0, 0x3fd0c9, 0x6fe0ff, 0xf28fb5, 0xf2d377,
           0x5fd6a0, 0x6fe0ff, 0xf28fb5),

        // abyss — deep-sea near-black blue, bioluminescent accents.
        mk("abyss", 0x040b18, 0xc9d9f5,
           0x2fe6a8, 0xf2d06b, 0xff9e5e, 0xff5c72,
           0x3aa0ff, 0x9a7bff, 0x39d9d0, 0x5fd0ff, 0xff7bd5, 0xf2d06b,
           0x2fe6a8, 0x5fd0ff, 0xff7bd5),

        // forest — deep pine green, moss and bark accents.
        mk("forest", 0x0e1a14, 0xdce8dc,
           0x7ec98f, 0xe4c579, 0xdd9b62, 0xd76a6a,
           0x6fb0a0, 0xba9adf, 0x86c9a8, 0x8fd0bd, 0xd7a0c0, 0xe4c579,
           0x7ec98f, 0x8fd0bd, 0xd7a0c0),

        // sakura — soft cherry-blossom dusk. Rose + plum on warm dark.
        mk("sakura", 0x1e1420, 0xf3e0ea,
           0x8fd6a8, 0xf2d491, 0xf3a678, 0xef6a8b,
           0x8fb8f0, 0xc79be8, 0x8fd6cf, 0xa9c8f5, 0xf6a5d8, 0xf2d491,
           0x8fd6a8, 0x8fd6cf, 0xf6a5d8),

        // ── Warm / cool minimalists ────────────────────────────────────────────
        // espresso — warm coffee browns with cream ink.
        mk("espresso", 0x231a15, 0xe8dcc8,
           0xa8c66c, 0xe6c86e, 0xe08b4a, 0xd85c5c,
           0x8fb4c9, 0xc39bd3, 0x86c6b8, 0x9fc9dd, 0xdb9bb8, 0xe6c86e,
           0xa8c66c, kDerive, 0xe08b4a),

        // midnight — cool slate-indigo, minimal and crisp.
        mk("midnight", 0x0f1220, 0xd7dcf0,
           0x6fd39a, 0xe4cd7c, 0xe6a15e, 0xe86c8a,
           0x6f9fef, 0xa88ceb, 0x5fc9c3, 0x77c6ff, 0xf08fd0, 0xe4cd7c,
           0x6fd39a, 0x77c6ff, 0xf08fd0),

        // graphite — near-monochrome slate with a single cyan signal.
        mk("graphite", 0x17181c, 0xd6d8dc,
           0x8bc99a, 0xd6c67e, 0xd99a63, 0xd76d6d,
           0x7fb0d8, 0xb098d0, 0x77c4bd, 0x8fc6e0, 0xcf98c0, 0xd6c67e,
           0x8bc99a, 0x8fc6e0, 0xcf98c0),

        // paper — clean light theme, high-contrast ink on off-white.
        mk("paper", 0xf4f1ea, 0x2b2b2b,
           0x2e7d32, 0xb28704, 0xd8620a, 0xc62828,
           0x1565c0, 0x6a1b9a, 0x00838f, 0x0277bd, 0xad1457, 0xb28704,
           0x2e7d32, kDerive, 0xad1457),
    };
    return s;
}

// Build the full deck once: native at index 0, then every derived spec.
inline const std::vector<Theme>& theme_table() {
    static const std::vector<Theme> t = [] {
        std::vector<Theme> v;
        v.reserve(theme_specs().size() + 1);
        v.push_back(native_theme());
        for (const auto& sp : theme_specs()) v.push_back(build_theme(sp));
        return v;
    }();
    return t;
}

}  // namespace detail

// ── the mutable active palette ──────────────────────────────────────────────
// A single mutable Theme the whole app reads through references. Seeded with
// "native". set_theme() copies a deck entry over it in place, so the inline
// `pal::` references below stay valid and simply see new values next frame.
inline Theme g_active = detail::theme_table()[0];
inline std::size_t g_active_idx = 0;

[[nodiscard]] inline std::size_t theme_count() { return detail::theme_table().size(); }
[[nodiscard]] inline const char* theme_name(std::size_t i) {
    const auto& t = detail::theme_table();
    return t[i % t.size()].name;
}
// The full palette of ANY deck entry (for the picker's per-row swatches) —
// read-only, distinct from g_active which is the LIVE theme.
[[nodiscard]] inline const Theme& theme_at(std::size_t i) {
    const auto& t = detail::theme_table();
    return t[i % t.size()];
}
[[nodiscard]] inline const char* active_theme_name() { return g_active.name; }
[[nodiscard]] inline std::size_t active_theme_index() { return g_active_idx; }

inline void set_theme(std::size_t idx) {
    const auto& tbl = detail::theme_table();
    idx %= tbl.size();
    g_active = tbl[idx];
    g_active_idx = idx;
    // (Every RGB theme's `bg` is a concrete dark badge-ink color set by
    // build_theme; `bg_panel` carries the true canvas. Native keeps default.)
}

// True when the active theme owns its own canvas — every RGB theme. The native
// theme (index 0) defers to the terminal, so it paints nothing.
[[nodiscard]] inline bool theme_paints_canvas() { return g_active_idx != 0; }
// The color to fill the root canvas with when theme_paints_canvas().
[[nodiscard]] inline maya::Color theme_canvas() { return g_active.bg_panel; }
// Cycle to the next/previous theme (T) — returns the new name for a toast.
inline const char* cycle_theme(int dir = +1) {
    const std::size_t n = detail::theme_table().size();
    const int step = ((dir % static_cast<int>(n)) + static_cast<int>(n)) % static_cast<int>(n);
    set_theme((g_active_idx + static_cast<std::size_t>(step)) % n);
    return g_active.name;
}
// Resolve a saved theme name to its index (-1 if unknown).
[[nodiscard]] inline int theme_index_by_name(const std::string& name) {
    const auto& tbl = detail::theme_table();
    for (std::size_t i = 0; i < tbl.size(); ++i)
        if (name == tbl[i].name) return static_cast<int>(i);
    return -1;
}

namespace pal {
// Every name below is a REFERENCE into g_active. Reading `pal::dim` reads the
// active theme's current `dim`; set_theme() mutates g_active in place, so the
// references never dangle and every call site tracks the live theme.
inline maya::Color& bg       = g_active.bg;
inline maya::Color& bg_panel = g_active.bg_panel;
inline maya::Color& border   = g_active.border;
inline maya::Color& track    = g_active.track;   // meter groove
inline maya::Color& rail     = g_active.rail;    // table header band
inline maya::Color& sel_bg   = g_active.sel_bg;  // selected-row strip

inline maya::Color& white    = g_active.white;
inline maya::Color& text     = g_active.text;    // normal fg
inline maya::Color& label    = g_active.label;
inline maya::Color& dim      = g_active.dim;
inline maya::Color& faint    = g_active.faint;

inline maya::Color& good     = g_active.good;
inline maya::Color& warn     = g_active.warn;
inline maya::Color& hot      = g_active.hot;     // orange rung
inline maya::Color& crit     = g_active.crit;
inline maya::Color& blue     = g_active.blue;
inline maya::Color& mauve    = g_active.mauve;
inline maya::Color& teal     = g_active.teal;
inline maya::Color& sky      = g_active.sky;
inline maya::Color& pink     = g_active.pink;
inline maya::Color& amber    = g_active.amber;

inline maya::Color& cpu_ac   = g_active.cpu_ac;
inline maya::Color& mem_ac   = g_active.mem_ac;
inline maya::Color& disk_ac  = g_active.disk_ac;
inline maya::Color& net_ac   = g_active.net_ac;
inline maya::Color& gpu_ac   = g_active.gpu_ac;
inline maya::Color& proc_ac  = g_active.proc_ac;
}  // namespace pal

// A "blend" picks one endpoint by which side of the midpoint t falls on — but
// when BOTH colors are truecolor (the RGB themes), interpolate for real so
// subtle tints (mix(dim, bg, 0.35)) read as intended instead of snapping.
[[nodiscard]] inline maya::Color mix(maya::Color a, maya::Color b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    if (a.kind() == maya::Color::Kind::Rgb && b.kind() == maya::Color::Kind::Rgb) {
        auto lerp = [t](std::uint8_t x, std::uint8_t y) {
            return static_cast<std::uint8_t>(x + (static_cast<double>(y) - x) * t + 0.5);
        };
        return maya::Color::rgb(lerp(a.r(), b.r()), lerp(a.g(), b.g()), lerp(a.b(), b.b()));
    }
    return t < 0.5 ? a : b;
}

// Lift a color toward white. For truecolor themes that's a real lighten; for
// named ANSI slots it promotes to the bright counterpart (green → bright green)
// exactly as before. bright_black lifts to white so it stays visible on a
// selection strip that is also bright_black.
[[nodiscard]] inline maya::Color brighten(maya::Color c) {
    if (c.kind() == maya::Color::Kind::Rgb) return c.lighten(0.35f);
    if (c.kind() == maya::Color::Kind::Named) {
        if (c.index() < 8)
            return maya::Color{static_cast<maya::AnsiColor>(c.index() + 8)};
        if (c.index() == 8)   // bright_black → white: visible on the strip
            return maya::Color::white();
    }
    return c;
}

// Load ramp — steps through the theme's semantic slots (no gradient: the
// four rungs are chosen to stay distinct in every theme).
[[nodiscard]] inline maya::Color load_color(double f) {
    f = std::clamp(f, 0.0, 1.0);
    if (f < 0.55) return pal::good;   // green
    if (f < 0.80) return pal::warn;   // yellow
    if (f < 0.92) return pal::hot;    // orange
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
