// theme.hpp — rockbottom's palette, projected from maya's theme registry.
//
// HISTORY: rb used to carry its own deck of 35 hand-authored `Spec`s (a bg, an
// fg, a four-rung ramp, six accents) and derive a palette from each. maya now
// ships a central theme system — 615 named schemes in maya/style/schemes.hpp,
// each a 24-slot semantic `maya::Theme` — so the hand-authored deck is gone.
// Keeping it meant re-tuning 35 palettes by hand forever while a larger,
// better-curated set sat one include away.
//
// WHAT DID NOT CHANGE, and why:
//
//   1. `pal::` stays. ~940 call sites across 28 files read `pal::dim`,
//      `pal::cpu_ac`, `pal::good`. They are inline references into one
//      mutable `g_active`, so a theme swap copies new values in and every
//      site sees them next frame. Rewriting 940 sites to speak maya's slot
//      names would be a far bigger diff for no gain.
//
//   2. The derivation math stays. maya's Theme is a vocabulary for an APP:
//      primary, surface, border, error, selection. rockbottom needs a
//      MONITOR's vocabulary — a four-rung load ramp whose rungs stay
//      ordinally distinct at a glance, five ink tiers, and six per-domain
//      signature accents. Those are not in maya's 24 slots and shouldn't be:
//      no other maya app needs "the colour of a disk that is nearly full".
//
// So this file is a PROJECTION now. maya owns the colours, rb owns the
// interpretation: project_theme() maps maya's 24 slots onto rb's 32 and
// derives the remainder with the same contrast math build_theme() used, so all
// 615 schemes get structurally identical legibility for free. That is what
// maya::theme::Projection is for.
//
// CONTRAST: surfaces step UP from the canvas toward the ink; ink tiers step
// DOWN from the text colour toward the canvas; the load ramp is re-ordered
// when a scheme's own warning/error hues would read out of sequence. Light
// schemes are detected by luma and handled explicitly rather than hoped about.

#pragma once

#include <maya/maya.hpp>
#include <maya/style/schemes.hpp>

#include "../core/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rockbottom::ui {

// ── the semantic palette ────────────────────────────────────────────────────
// Every field is a maya::LitColor — a RESOLVED colour. maya 0.5 split the
// colour type in two: `Color` (BasicColor<Res::Sym>) may carry a symbolic
// theme slot that still needs resolving, while `LitColor` is a literal value
// whose channels are real and safe to do arithmetic on. Projection resolves
// every slot up front (see project_theme), so everything here is literal by
// construction — which is what lets mix() and brighten() read .r()/.g()/.b()
// at all, those accessors being Lit-only precisely so nobody can blend an
// unresolved slot and paint a near-black triple. LitColor widens to Color
// implicitly, so every call site that hands one to maya keeps working.
struct Theme {
    const char* name;

    // Surfaces / structure.
    maya::LitColor bg, bg_panel, border, track, rail, sel_bg;
    // Ink tiers.
    maya::LitColor white, text, label, dim, faint;
    // Semantic ramp + spectral accents.
    maya::LitColor good, warn, hot, crit;
    maya::LitColor blue, mauve, teal, sky, pink, amber;
    // Per-domain signature accents.
    maya::LitColor cpu_ac, mem_ac, disk_ac, net_ac, gpu_ac, proc_ac;

    // maya::theme::Projection requires the projected type to be
    // equality-comparable: it caches one snapshot and compares before
    // republishing, so an unchanged theme costs nothing.
    bool operator==(const Theme&) const = default;
};

namespace detail {
using C = maya::LitColor;

// ── RGB helpers (host-side, so they can use float math freely) ───────────────
struct Rgb { double r, g, b; };  // channels in 0..255

// Channels out of a maya colour.
//
// to_rgb() is the load-bearing call here, not .r()/.g()/.b(). A scheme slot may
// be Named or Indexed (maya's `native` theme is entirely named ANSI slots), and
// on those the raw accessors return a PALETTE INDEX living in the red byte —
// reading them as channels is how you end up painting bright_black as
// rgb(8,0,0). to_rgb() maps a named/indexed colour to its real triple, so the
// derivation below can safely do arithmetic on any of the 615 schemes.
[[nodiscard]] inline Rgb chan(maya::LitColor c) {
    const maya::LitColor lit = c.to_rgb();
    return {static_cast<double>(lit.r()),
            static_cast<double>(lit.g()),
            static_cast<double>(lit.b())};
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

// Saturate a hue while holding its luma — for a ramp rung that must stay
// readable but needs to look more urgent than the rung below it.
[[nodiscard]] inline Rgb vivid(Rgb c, double amount) {
    const double l = luma(c);
    return {l + (c.r - l) * amount, l + (c.g - l) * amount, l + (c.b - l) * amount};
}

// Project one of maya's 24-slot schemes onto rockbottom's 32-slot palette.
//
// Conservative where maya has an opinion, derived where it doesn't:
//
//   maya slot                  →  rb use
//   background                    the canvas
//   text                          primary ink (all five tiers derive from it)
//   border / surface / selection  structure, when the scheme distinguishes them
//   success / warning / error     three of the four load-ramp rungs
//   primary / secondary / …       the six spectral accents
//
// The FOURTH rung has no maya equivalent, and that gap is the interesting part
// of this function. A monitor has to distinguish "bad" from "actively on
// fire"; an app theme has exactly one error colour. So `crit` is `error`
// pushed further — saturated and lifted off the canvas — which keeps it
// ordinally distinct from `hot` on every scheme instead of being the same red
// printed twice.
[[nodiscard]] inline Theme project_theme(const char* name, const maya::Theme& m) {
    const Rgb bg = chan(m.background);
    const Rgb fg = chan(m.text);
    // Lift bg toward fg for surface tiers (panels/borders sit above the canvas).
    auto up   = [&](double t) { return from_rgb(lerp(bg, fg, t)); };
    // Pull fg toward bg for ink tiers (dimmer text recedes into the canvas).
    auto down = [&](double t) { return from_rgb(lerp(fg, bg, t)); };

    Theme t{};
    t.name = name;

    // Does the scheme distinguish this slot from the canvas? Several minimal
    // schemes set surface/border equal to the background, and trusting those
    // blindly gives invisible panel outlines — so fall back to our own step.
    auto differs = [&](maya::LitColor c, double by) {
        return std::abs(luma(chan(c)) - luma(bg)) > by;
    };

    // Surfaces — canvas, then progressively lighter structure.
    // `bg` is ALSO the punch-out badge ink (fgc(pal::bg) over a bright chip),
    // so it must read dark against the accent chips. On a LIGHT theme the
    // canvas is bright, so a bright `bg` would make badge text invisible; keep
    // badge ink dark there while `bg_panel` stays the true (light) canvas.
    const bool light = luma(bg) > 140.0;
    t.bg       = light ? from_rgb(lerp(bg, {0, 0, 0}, 0.82)) : from_rgb(bg);
    t.bg_panel = from_rgb(bg);        // the real canvas (painted by the app)
    // maya authors border/surface/selection per scheme, which is better
    // evidence than a blend of ours — prefer them when they say something.
    t.border = differs(m.border, 6.0)    ? from_rgb(chan(m.border))    : up(0.22);
    t.track  = differs(m.surface, 4.0)   ? from_rgb(chan(m.surface))   : up(0.14);
    t.rail   = differs(m.surface, 4.0)   ? from_rgb(chan(m.surface))   : up(0.16);
    t.sel_bg = differs(m.selection, 4.0) ? from_rgb(chan(m.selection)) : up(0.20);

    // Ink tiers — bright headline down to faint structure. On a light theme
    // "brighten toward white" would REDUCE contrast, so the headline moves
    // toward black instead; dim/faint recede toward the (light) canvas.
    t.white = light ? from_rgb(lerp(fg, {0, 0, 0}, 0.25))
                    : from_rgb(lerp(fg, {255, 255, 255}, 0.35));
    t.text  = from_rgb(fg);
    t.label = down(0.14);
    t.dim   = down(0.52);             // structure ink — legible but recessive
    t.faint = down(0.66);             // faintest guide lines

    // The load ramp: good → warn → hot → crit, and it MUST read in that order
    // at a glance or the whole "glance at it and know" premise fails.
    //
    // maya gives us three of the four. `hot` sits between warning and error
    // (amber-into-red); `crit` is error made more urgent than error.
    //
    // But a scheme is NOT required to make those three distinct — it is
    // authored for an editor, where success/warning/error rarely sit adjacent.
    // "HaX0R Blue" sets all three to the same #10b6ff, and a few dozen others
    // separate them by only a few units. Painted straight through, rb's four
    // rungs would be one colour and a full disk would look like an idle one.
    // So the ramp is CONSTRUCTED to be ordinal, using the scheme's hues as
    // evidence rather than as the answer: derive, then enforce separation.
    const Rgb ok  = chan(m.success);
    const Rgb wrn = chan(m.warning);
    const Rgb err = chan(m.error);
    const Rgb away = light ? Rgb{0, 0, 0} : Rgb{255, 255, 255};

    // Perceptual-ish distance; the channel weights approximate how much each
    // contributes to a difference the eye actually notices.
    auto sep = [](Rgb a, Rgb b) {
        const double dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
        return std::sqrt(2 * dr * dr + 4 * dg * dg + 3 * db * db);
    };

    Rgb r_good = ok;
    Rgb r_warn = wrn;
    Rgb r_hot  = lerp(wrn, err, 0.55);
    Rgb r_crit = vivid(lerp(err, away, 0.18), 1.25);

    // Test the ADJACENT PAIRS, which is what the eye actually compares — and
    // test them on the DERIVED rungs, not on the scheme's raw slots. An
    // earlier version compared warn against crit and passed HaX0R Blue (all
    // three slots #10b6ff), because vivid() had already pulled crit far enough
    // from warn to look fine while warn/hot and hot/crit stayed identical.
    //
    // The rebuild spreads the top three along a canonical amber→orange→red
    // path anchored on the scheme's error hue, so the theme keeps its
    // character while the rungs can be told apart.
    const double kMinSep = 45.0;
    if (sep(r_warn, r_hot) < kMinSep || sep(r_hot, r_crit) < kMinSep) {
        r_warn = lerp(err, Rgb{255, 190, 60}, 0.55);   // toward amber
        r_hot  = lerp(err, Rgb{255, 120, 40}, 0.35);   // toward orange
        r_crit = vivid(lerp(err, away, 0.10), 1.35);   // the hottest
    }
    // A scheme whose success IS its warning gives `good` nothing to be: push
    // it to whichever of the scheme's cool hues stands furthest off warn.
    if (sep(r_good, r_warn) < kMinSep) {
        const Rgb cool = chan(m.info);
        r_good = sep(cool, r_warn) > sep(r_good, r_warn)
                     ? cool
                     : lerp(ok, Rgb{120, 220, 120}, 0.6);
    }
    // Final pass on hot↔crit specifically.
    //
    // These two are the hardest pair and the most important one: they are the
    // difference between "this disk is filling up" and "this disk is about to
    // stop the machine". They also collide most easily, because both derive
    // from the same `error` hue — vivid() can only push so far when error is
    // already saturated (a pure #ff0000 has nowhere left to go).
    //
    // So when they still read the same, separate them by BRIGHTNESS rather
    // than hue: drop `hot` toward the canvas and keep `crit` at full strength.
    // A dimmer amber under a blazing red is still unambiguous, and it is a
    // change the eye reads even when both are the same hue family.
    for (int pass = 0; pass < 3 && sep(r_hot, r_crit) < kMinSep; ++pass) {
        r_hot  = lerp(r_hot, bg, 0.22);
        r_crit = vivid(lerp(r_crit, away, 0.10), 1.15);
    }

    t.good = from_rgb(r_good);
    t.warn = from_rgb(r_warn);
    t.hot  = from_rgb(r_hot);
    t.crit = from_rgb(r_crit);

    // Spectral accents, straight off maya's semantic slots.
    t.blue  = from_rgb(chan(m.primary));
    t.mauve = from_rgb(chan(m.accent));
    t.teal  = from_rgb(chan(m.secondary));
    t.sky   = from_rgb(chan(m.info));
    t.pink  = from_rgb(chan(m.link));
    t.amber = from_rgb(wrn);

    // Domain accents — six panels that must stay mutually distinguishable:
    // cpu blue · mem mauve · disk teal · net green · gpu sky · proc pink.
    t.cpu_ac  = t.blue;
    t.mem_ac  = t.mauve;
    t.disk_ac = t.teal;
    t.net_ac  = t.good;
    t.gpu_ac  = t.sky;
    t.proc_ac = t.pink;

    // Some schemes are MONOCHROME BY DESIGN — "HaX0R Blue", "HaX0R Gr33N",
    // "Retro" — and set every semantic slot to one hue. That is a legitimate
    // aesthetic for an editor, but here it means the cpu graph, the mem graph
    // and the net graph are the same colour and the panels stop being
    // tellable apart.
    //
    // Dropping those themes would be the easy answer and the wrong one: the
    // whole point of adopting maya's registry is that the user picks, not us.
    // So keep the hue and separate by BRIGHTNESS, fanning the six accents
    // along a light→dark ladder from the scheme's own colour. A green monitor
    // stays a green monitor; its six panels just stop being one green.
    {
        const maya::LitColor acc[6] = {t.cpu_ac, t.mem_ac, t.disk_ac,
                                       t.net_ac, t.gpu_ac, t.proc_ac};
        int identical = 0;
        for (int a = 0; a < 6; ++a)
            for (int b = a + 1; b < 6; ++b) {
                const Rgb x = chan(acc[a]), y = chan(acc[b]);
                if (std::abs(x.r - y.r) + std::abs(x.g - y.g) + std::abs(x.b - y.b) < 2.0)
                    ++identical;
            }
        // 15 pairs = all six the same. Rebuild well before that (8 pairs is
        // already "most of them collide").
        if (identical >= 8) {
            const Rgb base = chan(t.cpu_ac);
            // Fan across the canvas→base→away axis so every step stays on the
            // scheme's hue while landing at a visibly different brightness.
            const double steps[6] = {0.00, 0.34, -0.26, 0.62, -0.48, 0.16};
            maya::LitColor* slot[6] = {&t.cpu_ac, &t.mem_ac, &t.disk_ac,
                                       &t.net_ac, &t.gpu_ac, &t.proc_ac};
            for (int i = 0; i < 6; ++i) {
                const double s = steps[i];
                *slot[i] = s >= 0 ? from_rgb(lerp(base, away, s))
                                  : from_rgb(lerp(base, bg, -s));
            }
        }
    }
    return t;
}
// ── the deck: maya's scheme registry ────────────────────────────────────────
// This used to be ~230 lines of hand-authored Spec literals. It is now a view
// over maya::theme::schemes[], which is 615 curated schemes (Catppuccin,
// Gruvbox, Nord, Tokyo Night, Dracula, Solarized, the Alacritty/iTerm/Windows
// Terminal collections, and a lot more) sorted by name.
//
// Index 0 is ALWAYS "native": every slot a named ANSI-16 colour, so the
// terminal owns the hues and we never paint the canvas. It has to stay first
// and stay hand-held because it is the only theme that is deliberately NOT a
// palette — it is the absence of one, and the fallback when a saved name no
// longer resolves.
//
// Projection is LAZY and cached. Building all 615 palettes up front would be
// ~600 × 32 blends on a startup path that shows one theme, so entries hold the
// maya scheme pointer and materialise an rb Theme on first read. The picker
// scrolling through the list warms only the rows it paints.
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

// One deck slot: a name, and where its colours come from.
struct Slot {
    const char*        name;
    const maya::Theme* scheme;   // null = native (not a projection)
};

// The deck, in display order: native, then maya's schemes by name.
inline const std::vector<Slot>& deck() {
    static const std::vector<Slot> d = [] {
        std::vector<Slot> v;
        v.reserve(std::size(maya::theme::schemes) + 1);
        v.push_back({"native", nullptr});
        for (const auto& s : maya::theme::schemes) v.push_back({s.name, s.theme});
        return v;
    }();
    return d;
}

// Materialise deck entry `i`, memoised. Returning a reference keeps the
// ~940 `pal::` sites and the picker's swatches cheap on repeat reads.
[[nodiscard]] inline const Theme& deck_theme(std::size_t i) {
    static std::vector<Theme> cache(deck().size());
    static std::vector<char>  built(deck().size(), 0);
    if (i >= deck().size()) i = 0;
    if (!built[i]) {
        const Slot& s = deck()[i];
        cache[i] = s.scheme ? project_theme(s.name, *s.scheme) : native_theme();
        built[i] = 1;
    }
    return cache[i];
}

}  // namespace detail

// ── the mutable active palette ──────────────────────────────────────────────
// A single mutable Theme the whole app reads through references. Seeded with
// "native". set_theme() copies a deck entry over it in place, so the inline
// `pal::` references below stay valid and simply see new values next frame.
inline Theme g_active = detail::deck_theme(0);
inline std::size_t g_active_idx = 0;

[[nodiscard]] inline std::size_t theme_count() { return detail::deck().size(); }
[[nodiscard]] inline const char* theme_name(std::size_t i) {
    const auto& d = detail::deck();
    return d[i % d.size()].name;
}
// The full palette of ANY deck entry (for the picker's per-row swatches) —
// read-only, distinct from g_active which is the LIVE theme.
[[nodiscard]] inline const Theme& theme_at(std::size_t i) {
    return detail::deck_theme(i % detail::deck().size());
}
[[nodiscard]] inline const char* active_theme_name() { return g_active.name; }
[[nodiscard]] inline std::size_t active_theme_index() { return g_active_idx; }

inline void set_theme(std::size_t idx) {
    idx %= detail::deck().size();
    g_active = detail::deck_theme(idx);
    g_active_idx = idx;
    // Every projected theme's `bg` is a concrete dark badge-ink colour set by
    // project_theme, and `bg_panel` carries the true canvas. Native keeps
    // default_color for both, so it paints nothing.
    //
    // Also publish to maya: any maya WIDGET we draw (and maya's own
    // projections, e.g. the markdown palette) resolves its symbolic slots
    // against theme::live(). Without this the app's own panels would follow
    // the picker while anything maya draws for us stayed on the default
    // scheme — a split-brain palette. native has no maya scheme behind it, so
    // it publishes maya's own `native`, which is the same intent: defer.
    const detail::Slot& s = detail::deck()[idx];
    maya::theme::set_live(s.scheme ? *s.scheme : maya::theme::native);
}

// True when the active theme owns its own canvas — every projected theme. The
// native theme (index 0) defers to the terminal, so it paints nothing.
[[nodiscard]] inline bool theme_paints_canvas() { return g_active_idx != 0; }
// The color to fill the root canvas with when theme_paints_canvas().
[[nodiscard]] inline maya::LitColor theme_canvas() { return g_active.bg_panel; }
// Cycle to the next/previous theme (T) — returns the new name for a toast.
inline const char* cycle_theme(int dir = +1) {
    const std::size_t n = detail::deck().size();
    const int step = ((dir % static_cast<int>(n)) + static_cast<int>(n)) % static_cast<int>(n);
    set_theme((g_active_idx + static_cast<std::size_t>(step)) % n);
    return g_active.name;
}
// Resolve a saved theme name to its index (-1 if unknown).
//
// Case- and separator-insensitive: the deck's display names are maya's
// ("Tokyo Night", "Catppuccin Mocha"), but a config file or a --theme= flag is
// typed by a human who will write `tokyo-night` or `tokyo_night`. An exact
// match always wins; the loose pass is the fallback.
[[nodiscard]] inline int theme_index_by_name(const std::string& name) {
    const auto& d = detail::deck();
    for (std::size_t i = 0; i < d.size(); ++i)
        if (name == d[i].name) return static_cast<int>(i);
    auto norm = [](std::string_view s) {
        std::string o;
        for (char c : s) {
            if (c == ' ' || c == '-' || c == '_' || c == '.') continue;
            o += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
        return o;
    };
    const std::string want = norm(name);
    if (want.empty()) return -1;
    for (std::size_t i = 0; i < d.size(); ++i)
        if (want == norm(d[i].name)) return static_cast<int>(i);
    return -1;
}
// ── searching the deck ──────────────────────────────────────────────────
// With 600+ themes, browsing is search, not scrolling. Both the picker's
// filter box and the CLI's "did you mean" use the SAME matcher, so what you
// type in one behaves like the other.
//
// Matching is subsequence-based, not substring: "cmocha" finds "Catppuccin
// Mocha" and "tnsto" finds "TokyoNight Storm". Scoring prefers, in order, a
// prefix hit, then a word-boundary hit, then a tight cluster of matched
// characters — so typing "nord" puts "Nord" above "Nordfox" above the
// incidental matches.
namespace detail {

// Normalise for matching: lowercase, and drop the separators humans are
// inconsistent about.
[[nodiscard]] inline std::string fold(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '-' || c == '_' || c == '.' || c == '(' || c == ')') continue;
        o += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    return o;
}

// Score `name` against a folded query. Higher is better; -1 means no match.
[[nodiscard]] inline int match_score(std::string_view name, const std::string& q) {
    if (q.empty()) return 0;
    const std::string f = fold(name);
    if (f.empty()) return -1;

    // Exact and prefix are special-cased so they can never lose to a
    // scattered subsequence in a longer name.
    if (f == q) return 10000;
    if (f.compare(0, q.size(), q) == 0) return 5000 - static_cast<int>(f.size());

    // Subsequence walk, rewarding adjacency and word starts.
    std::size_t qi = 0;
    int score = 0, run = 0;
    bool at_word_start = true;
    for (std::size_t i = 0; i < name.size() && qi < q.size(); ++i) {
        const char raw = name[i];
        const bool sep = raw == ' ' || raw == '-' || raw == '_' || raw == '.'
                      || raw == '(' || raw == ')';
        if (sep) { at_word_start = true; continue; }
        const char lc = static_cast<char>(raw >= 'A' && raw <= 'Z' ? raw - 'A' + 'a' : raw);
        if (lc == q[qi]) {
            ++qi;
            run += 1;
            score += 10 + run * 4 + (at_word_start ? 25 : 0);
        } else {
            run = 0;
        }
        at_word_start = false;
    }
    if (qi < q.size()) return -1;                 // not all of the query landed
    return score - static_cast<int>(f.size());    // prefer the shorter name
}

}  // namespace detail

// Deck indices matching `query`, best first. An empty query means "all of it,
// in deck order" — which is what the picker shows before you type.
[[nodiscard]] inline std::vector<std::size_t> theme_search(const std::string& query) {
    const auto& d = detail::deck();
    std::vector<std::size_t> out;
    const std::string q = detail::fold(query);
    if (q.empty()) {
        out.resize(d.size());
        for (std::size_t i = 0; i < d.size(); ++i) out[i] = i;
        return out;
    }
    std::vector<std::pair<int, std::size_t>> hits;
    for (std::size_t i = 0; i < d.size(); ++i)
        if (const int s = detail::match_score(d[i].name, q); s >= 0)
            hits.push_back({s, i});
    // Stable by score, then by deck order, so the list never reshuffles
    // between two equally good matches.
    std::stable_sort(hits.begin(), hits.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    out.reserve(hits.size());
    for (const auto& [s, i] : hits) out.push_back(i);
    return out;
}

// Up to `limit` theme names close to `query`, for a CLI "did you mean".
//
// Falls back to bigram overlap when the subsequence matcher finds nothing,
// which is the TYPO case: "mocah" has its letters out of order, so no
// subsequence matches, but it shares most of its bigrams with "mocha". Only
// the error path pays for this.
[[nodiscard]] inline std::vector<std::string> theme_suggestions(const std::string& query,
                                                                std::size_t limit) {
    std::vector<std::string> out;
    for (std::size_t i : theme_search(query)) {
        if (out.size() >= limit) break;
        out.push_back(theme_name(i));
    }
    if (!out.empty()) return out;

    const std::string q = detail::fold(query);
    if (q.size() < 2) return out;
    auto bigrams = [](const std::string& s) {
        std::vector<std::string> v;
        for (std::size_t i = 0; i + 1 < s.size(); ++i) v.push_back(s.substr(i, 2));
        return v;
    };
    const std::vector<std::string> qb = bigrams(q);
    const auto& d = detail::deck();
    std::vector<std::pair<int, std::size_t>> scored;
    for (std::size_t i = 0; i < d.size(); ++i) {
        const std::string f = detail::fold(d[i].name);
        int shared = 0;
        for (const std::string& b : qb)
            if (f.find(b) != std::string::npos) ++shared;
        // Demand real overlap, or every theme "matches" a two-letter typo.
        if (shared * 2 >= static_cast<int>(qb.size())) scored.push_back({shared, i});
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [s, i] : scored) {
        if (out.size() >= limit) break;
        out.push_back(theme_name(i));
    }
    return out;
}

// Resolve a name the way a HUMAN typed it, for --theme= and the config file.
//
// Three passes, narrowest first, so the answer is never a surprise:
//   1. exact         "Catppuccin Mocha"
//   2. loose         case/separator-insensitive: "catppuccin-mocha"
//   3. unambiguous   a subsequence that matches exactly ONE theme: "cmocha"
//
// Pass 3 is what makes short names usable against a 616-entry deck, and the
// uniqueness requirement is what keeps it safe: "gruv" matches seven themes,
// so it is REJECTED with suggestions rather than silently resolving to
// whichever one happened to rank first. Guessing there would mean a config
// file could change meaning when maya adds a scheme.
[[nodiscard]] inline int theme_resolve(const std::string& name) {
    if (const int i = theme_index_by_name(name); i >= 0) return i;
    const std::vector<std::size_t> hits = theme_search(name);
    if (hits.size() == 1) return static_cast<int>(hits[0]);
    return -1;
}


namespace pal {
// Every name below is a REFERENCE into g_active. Reading `pal::dim` reads the
// active theme's current `dim`; set_theme() mutates g_active in place, so the
// references never dangle and every call site tracks the live theme.
inline maya::LitColor& bg       = g_active.bg;
inline maya::LitColor& bg_panel = g_active.bg_panel;
inline maya::LitColor& border   = g_active.border;
inline maya::LitColor& track    = g_active.track;   // meter groove
inline maya::LitColor& rail     = g_active.rail;    // table header band
inline maya::LitColor& sel_bg   = g_active.sel_bg;  // selected-row strip

inline maya::LitColor& white    = g_active.white;
inline maya::LitColor& text     = g_active.text;    // normal fg
inline maya::LitColor& label    = g_active.label;
inline maya::LitColor& dim      = g_active.dim;
inline maya::LitColor& faint    = g_active.faint;

inline maya::LitColor& good     = g_active.good;
inline maya::LitColor& warn     = g_active.warn;
inline maya::LitColor& hot      = g_active.hot;     // orange rung
inline maya::LitColor& crit     = g_active.crit;
inline maya::LitColor& blue     = g_active.blue;
inline maya::LitColor& mauve    = g_active.mauve;
inline maya::LitColor& teal     = g_active.teal;
inline maya::LitColor& sky      = g_active.sky;
inline maya::LitColor& pink     = g_active.pink;
inline maya::LitColor& amber    = g_active.amber;

inline maya::LitColor& cpu_ac   = g_active.cpu_ac;
inline maya::LitColor& mem_ac   = g_active.mem_ac;
inline maya::LitColor& disk_ac  = g_active.disk_ac;
inline maya::LitColor& net_ac   = g_active.net_ac;
inline maya::LitColor& gpu_ac   = g_active.gpu_ac;
inline maya::LitColor& proc_ac  = g_active.proc_ac;
}  // namespace pal

// A "blend" picks one endpoint by which side of the midpoint t falls on — but
// when BOTH colors are truecolor (the RGB themes), interpolate for real so
// subtle tints (mix(dim, bg, 0.35)) read as intended instead of snapping.
// Blend two palette colors. Delegates to maya's anim::lerp, which is the same
// componentwise interpolation this used to hand-roll PLUS one guard we were
// missing: it blends only when BOTH endpoints have real channels, and snaps to
// the nearer endpoint otherwise.
//
// That guard matters. On a Named color, r() is the palette INDEX and g()/b()
// are zero, so the old arithmetic turned bright_black (Named 8) into
// rgb(8,0,0) — a near-black triple, invisible on a dark terminal. Under the
// `native` theme, where every slot is deliberately Named so the user's own
// palette reaches the screen, every mix() was producing exactly that. An
// effect that cannot be computed has to become no effect, never a computed
// wrong answer.
[[nodiscard]] inline maya::LitColor mix(maya::LitColor a, maya::LitColor b, double t) {
    return maya::anim::lerp(a, b, std::clamp(t, 0.0, 1.0));
}

// Lift a color toward white. For truecolor themes that's a real lighten; for
// named ANSI slots it promotes to the bright counterpart (green → bright green)
// exactly as before. bright_black lifts to white so it stays visible on a
// selection strip that is also bright_black.
[[nodiscard]] inline maya::LitColor brighten(maya::LitColor c) {
    if (c.kind() == maya::ColorKind::Rgb) return c.lighten(0.35f);
    if (c.kind() == maya::ColorKind::Named) {
        if (c.index() < 8)
            return maya::LitColor{static_cast<maya::AnsiColor>(c.index() + 8)};
        if (c.index() == 8)   // bright_black → white: visible on the strip
            return maya::LitColor::white();
    }
    return c;
}

// Load ramp — steps through the theme's semantic slots (no gradient: the
// four rungs are chosen to stay distinct in every theme).
[[nodiscard]] inline maya::LitColor load_color(double f) {
    f = std::clamp(f, 0.0, 1.0);
    if (f < 0.55) return pal::good;   // green
    if (f < 0.80) return pal::warn;   // yellow
    if (f < 0.92) return pal::hot;    // orange
    return pal::crit;                 // red
}

[[nodiscard]] inline maya::LitColor health_color(Health h) {
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
