// units.hpp — Type-theoretic strong units for system telemetry.
//
// A "system monitor" is fundamentally an exercise in *dimensional analysis*:
// bytes, hertz, degrees, ratios and rates flow through the program and the
// classic bug is mixing them up (dividing bytes by seconds and storing the
// result as bytes, comparing a percentage to a raw count, …). We make those
// mistakes *unrepresentable* with phantom-tagged strong types, then hand the
// UI layer human-formatting that is correct by construction.
//
// Everything here is constexpr, trivially copyable, and zero-overhead: a
// `Bytes` is layout-identical to its underlying integer.

#pragma once

#include <cstdint>
#include <compare>
#include <string>
#include <array>
#include <algorithm>

namespace rockbottom {

// ── Strong<Tag, Rep> ────────────────────────────────────────────────────────
// A quantity is a representation `Rep` labelled with a phantom `Tag`. Two
// Strong types with different tags never implicitly convert, so the compiler
// rejects `bytes + hertz`. Arithmetic that *stays within a dimension*
// (Bytes+Bytes, Bytes*scalar) is allowed; crossing dimensions is deliberately
// a free function that names the result (see rate()).

template <class Tag, class Rep>
struct Strong {
    Rep value{};

    constexpr Strong() = default;
    constexpr explicit Strong(Rep v) noexcept : value(v) {}

    constexpr auto operator<=>(const Strong&) const = default;

    constexpr Strong operator+(Strong o) const noexcept { return Strong{value + o.value}; }
    constexpr Strong operator-(Strong o) const noexcept { return Strong{value - o.value}; }
    constexpr Strong& operator+=(Strong o) noexcept { value += o.value; return *this; }

    // Scale by a dimensionless scalar — stays in-dimension.
    template <class S> constexpr Strong operator*(S s) const noexcept { return Strong{static_cast<Rep>(value * s)}; }
    template <class S> constexpr Strong operator/(S s) const noexcept { return Strong{static_cast<Rep>(value / s)}; }

    // Ratio of two like quantities *is* dimensionless.
    constexpr double operator/(Strong o) const noexcept {
        return o.value == Rep{} ? 0.0 : static_cast<double>(value) / static_cast<double>(o.value);
    }
};

// ── Dimensions ──────────────────────────────────────────────────────────────

struct BytesTag {};
struct HertzTag {};
struct TicksTag {};   // CPU jiffies from /proc/stat
struct MilliTag {};   // milli-degrees / millivolts style raw kernel values

using Bytes = Strong<BytesTag, std::uint64_t>;
using Hertz = Strong<HertzTag, std::uint64_t>;
using Ticks = Strong<TicksTag, std::uint64_t>;

// ── Ratio ───────────────────────────────────────────────────────────────────
// A clamped [0,1] fraction. Constructed from a division so the *intent*
// (this is a proportion, not a count) is explicit at the call site.

struct Ratio {
    double v{};  // invariant: 0 <= v <= 1

    constexpr Ratio() = default;
    constexpr explicit Ratio(double d) noexcept : v(d < 0 ? 0 : d > 1 ? 1 : d) {}

    static constexpr Ratio of(double num, double den) noexcept {
        return den == 0 ? Ratio{0} : Ratio{num / den};
    }
    template <class T, class R>
    static constexpr Ratio of(Strong<T, R> a, Strong<T, R> b) noexcept {
        return Ratio{a / b};  // uses the dimensionless ratio operator
    }

    constexpr double percent() const noexcept { return v * 100.0; }
    constexpr auto operator<=>(const Ratio&) const = default;
};

// ── Rate<Q> ─────────────────────────────────────────────────────────────────
// A quantity-per-second. Crossing from Bytes to Bytes/s is an explicit,
// named construction — you cannot accidentally store a rate as a total.

template <class Q>
struct Rate {
    double per_sec{};
    constexpr Rate() = default;
    constexpr explicit Rate(double p) noexcept : per_sec(p) {}
};

using ByteRate = Rate<Bytes>;

// delta over dt seconds → rate. The only sanctioned Bytes→ByteRate crossing.
constexpr ByteRate rate(Bytes delta, double dt_sec) noexcept {
    return dt_sec <= 0 ? ByteRate{0} : ByteRate{static_cast<double>(delta.value) / dt_sec};
}

// ── Human formatting (correct by construction) ──────────────────────────────

inline std::string humanize_bytes(std::uint64_t n, bool bits = false) {
    // Binary units for storage/memory; label reflects it.
    static constexpr std::array<const char*, 7> u{"B", "K", "M", "G", "T", "P", "E"};
    double d = static_cast<double>(n);
    std::size_t i = 0;
    while (d >= 1024.0 && i + 1 < u.size()) { d /= 1024.0; ++i; }
    char buf[32];
    if (d < 10 && i > 0) std::snprintf(buf, sizeof buf, "%.1f%s", d, u[i]);
    else                 std::snprintf(buf, sizeof buf, "%.0f%s", d, u[i]);
    (void)bits;
    return buf;
}

inline std::string humanize_bytes(Bytes b) { return humanize_bytes(b.value); }

inline std::string humanize_rate(ByteRate r) {
    // Network rates in SI-ish per-second; "/s" suffix makes the dimension visible.
    static constexpr std::array<const char*, 5> u{"B/s", "K/s", "M/s", "G/s", "T/s"};
    double d = r.per_sec;
    std::size_t i = 0;
    while (d >= 1024.0 && i + 1 < u.size()) { d /= 1024.0; ++i; }
    char buf[32];
    std::snprintf(buf, sizeof buf, d < 10 && i > 0 ? "%.1f%s" : "%.0f%s", d, u[i]);
    return buf;
}

inline std::string humanize_hz(Hertz hz) {
    double d = static_cast<double>(hz.value);
    if (d >= 1e9) { char b[24]; std::snprintf(b, sizeof b, "%.2fGHz", d / 1e9); return b; }
    if (d >= 1e6) { char b[24]; std::snprintf(b, sizeof b, "%.0fMHz", d / 1e6); return b; }
    char b[24]; std::snprintf(b, sizeof b, "%.0fHz", d); return b;
}

inline std::string humanize_duration(std::uint64_t sec) {
    std::uint64_t d = sec / 86400; sec %= 86400;
    std::uint64_t h = sec / 3600;  sec %= 3600;
    std::uint64_t m = sec / 60;    sec %= 60;
    char buf[48];
    // Cast to unsigned long long so the %llu specifiers are correct whether
    // uint64_t is `unsigned long` (Linux LP64) or `unsigned long long` (macOS).
    auto L = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
    if (d) std::snprintf(buf, sizeof buf, "%llud %02llu:%02llu:%02llu", L(d), L(h), L(m), L(sec));
    else   std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu", L(h), L(m), L(sec));
    return buf;
}

}  // namespace rockbottom
