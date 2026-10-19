// throttle.hpp — Linux thermal throttle detection, as pure logic.
//
// Temperature alone misses throttling on parts with a low trip point (issue
// #11), so we read what the kernel itself reports:
//
//   cpuN/thermal_throttle/{core,package}_throttle_{count,total_time_ms}
//       x86 only. count goes up when an event starts, total_time_ms when it
//       ends. Core counters are shared by SMT siblings and package counters by
//       the whole package, so we read each once.
//   cpufreq/policyN/scaling_max_freq vs cpuinfo_max_freq
//       a cap below the hw max. A power profile does this too, so it's
//       evidence, not a verdict on its own.
//   /sys/class/thermal/cooling_deviceN with type "Processor"
//       cur_state > 0 means the thermal framework is holding the cpu down.
//
// probe() runs once and turns the tree into a flat list of slots, one per
// value worth reading. Each sample is then just "read every slot" (the Linux
// side keeps the fds open and preads them) plus the pure math below.

#pragma once

#include "metrics.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rockbottom::throttle {

using ReadFile = std::function<std::string(const std::string& path)>;

inline constexpr std::uint64_t kMissing = ~std::uint64_t{0};

enum class Kind : std::uint8_t { CoreCount, CoreMs, PkgCount, PkgMs, FreqCap, Cooling };

struct Slot {
    std::string   path;
    Kind          kind;
    std::uint32_t cpus = 1;   // logical cpus behind this value
    std::uint64_t aux = 0;    // FreqCap: cpuinfo_max_freq kHz. Cooling: max_state
};

struct Plan {
    std::vector<Slot> slots;
    bool empty() const { return slots.empty(); }
};

// Leading unsigned integer of a sysfs value ("4400000\n"). kMissing if none.
inline std::uint64_t parse_u64(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    std::uint64_t v = 0;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    return ec == std::errc{} && p != s.data() ? v : kMissing;
}

// Walk sysfs once and decide which values to read every sample.
inline Plan probe(int n_cpus, const ReadFile& read) {
    Plan plan;
    const auto u64 = [&](const std::string& p) { return parse_u64(read(p)); };
    const std::string cpu_dir = "/sys/devices/system/cpu/cpu";

    // Thermal counters, deduped by (package, core) and by package.
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> core_slot;
    std::map<std::uint64_t, std::size_t> pkg_slot;
    for (int i = 0; i < n_cpus; ++i) {
        const std::string b = cpu_dir + std::to_string(i);
        const std::string t = b + "/thermal_throttle/";
        std::uint64_t pkg = u64(b + "/topology/physical_package_id");
        std::uint64_t core = u64(b + "/topology/core_id");
        // No topology (masked /sys): treat every cpu as its own core.
        if (pkg == kMissing) pkg = 0;
        if (core == kMissing) core = 1'000'000 + static_cast<std::uint64_t>(i);

        if (u64(t + "core_throttle_count") != kMissing) {
            auto [it, fresh] = core_slot.try_emplace({pkg, core}, plan.slots.size());
            if (fresh) {
                plan.slots.push_back({t + "core_throttle_count", Kind::CoreCount});
                plan.slots.push_back({t + "core_throttle_total_time_ms", Kind::CoreMs});
            } else {
                ++plan.slots[it->second].cpus;
                ++plan.slots[it->second + 1].cpus;
            }
        }
        if (u64(t + "package_throttle_count") != kMissing) {
            auto [it, fresh] = pkg_slot.try_emplace(pkg, plan.slots.size());
            if (fresh) {
                plan.slots.push_back({t + "package_throttle_count", Kind::PkgCount});
                plan.slots.push_back({t + "package_throttle_total_time_ms", Kind::PkgMs});
            } else {
                ++plan.slots[it->second].cpus;
                ++plan.slots[it->second + 1].cpus;
            }
        }
    }

    // Frequency caps, one per cpufreq policy. Policies are numbered by their
    // first cpu, so probing policy0..policyN-1 covers them all.
    for (int i = 0; i < n_cpus; ++i) {
        const std::string p = "/sys/devices/system/cpu/cpufreq/policy" + std::to_string(i);
        const std::uint64_t mx = u64(p + "/cpuinfo_max_freq");
        if (mx == kMissing || mx == 0 || u64(p + "/scaling_max_freq") == kMissing) continue;
        plan.slots.push_back({p + "/scaling_max_freq", Kind::FreqCap, 1, mx});
    }

    // "Processor" cooling devices. Stop after a run of gaps.
    for (int i = 0, misses = 0; i < 4096 && misses < 8; ++i) {
        const std::string b = "/sys/class/thermal/cooling_device" + std::to_string(i);
        std::string type = read(b + "/type");
        if (type.empty()) { ++misses; continue; }
        misses = 0;
        while (!type.empty() && (type.back() == '\n' || type.back() == ' ')) type.pop_back();
        if (type != "Processor") continue;
        const std::uint64_t mx = u64(b + "/max_state");
        if (mx == kMissing || mx == 0) continue;
        plan.slots.push_back({b + "/cur_state", Kind::Cooling, 1, mx});
    }
    return plan;
}

// What changed between two reads of the same plan.
struct Delta {
    std::uint32_t cpus = 0;     // logical cpus whose counters moved
    std::uint64_t events = 0;   // new events on the worst core/package
    std::uint64_t ms = 0;       // throttled time that ended, worst core/package
};

// Counters only grow; a drop (hotplug, driver reload) or a failed read is zero.
// Counter slots come in (count, ms) pairs, as probe() lays them out.
inline Delta diff(const Plan& plan, std::span<const std::uint64_t> prev,
                  std::span<const std::uint64_t> now) {
    const auto grew = [&](std::size_t i) -> std::uint64_t {
        return prev[i] != kMissing && now[i] != kMissing && now[i] > prev[i] ? now[i] - prev[i] : 0;
    };
    Delta d;
    std::uint32_t core_cpus = 0, pkg_cpus = 0;
    const std::size_t n = std::min({plan.slots.size(), prev.size(), now.size()});
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const Kind k = plan.slots[i].kind;
        if (k != Kind::CoreCount && k != Kind::PkgCount) continue;
        const std::uint64_t ev = grew(i), ms = grew(i + 1);
        ++i;
        if (!ev && !ms) continue;
        d.events = std::max(d.events, ev);
        d.ms = std::max(d.ms, ms);
        // Core counters cover disjoint cpus, so they add up. Package counters
        // overlap the cores, so take the biggest one.
        if (k == Kind::CoreCount) core_cpus += plan.slots[i].cpus;
        else                      pkg_cpus = std::max(pkg_cpus, plan.slots[i].cpus);
    }
    d.cpus = std::max(core_cpus, pkg_cpus);
    return d;
}

// Fill the parts of CpuThrottle that come from the current read alone.
inline void summarize(const Plan& plan, std::span<const std::uint64_t> now, CpuThrottle& out) {
    out.supported = !plan.empty();
    double worst = 2.0;
    const std::size_t n = std::min(plan.slots.size(), now.size());
    for (std::size_t i = 0; i < n; ++i) {
        const Slot& s = plan.slots[i];
        const std::uint64_t v = now[i];
        if (v == kMissing) continue;
        switch (s.kind) {
            case Kind::CoreCount:
            case Kind::PkgCount:
                out.since_boot = std::max(out.since_boot, v);
                break;
            case Kind::FreqCap: {
                const double r = static_cast<double>(v) / static_cast<double>(s.aux);
                if (r < worst) {
                    worst = r;
                    out.cap = Hertz{v * 1000};
                    out.max = Hertz{s.aux * 1000};
                }
                break;
            }
            case Kind::Cooling:
                out.cooling_cur += static_cast<int>(std::min(v, s.aux));
                out.cooling_max += static_cast<int>(s.aux);
                break;
            default: break;
        }
    }
}

// Deltas over the last `span`, so a burst stays visible for a while instead of
// flashing for one tick. Fixed ring: no allocation, oldest entry evicted.
class Window {
public:
    using clock = std::chrono::steady_clock;
    explicit Window(clock::duration span = std::chrono::seconds(60)) : span_(span) {}

    void push(clock::time_point t, const Delta& d) {
        while (len_ && t - at_[head_] > span_) { head_ = (head_ + 1) % kCap; --len_; }
        if (!d.cpus && !d.events && !d.ms) return;
        if (len_ == kCap) { head_ = (head_ + 1) % kCap; --len_; }
        const std::size_t i = (head_ + len_) % kCap;
        at_[i] = t;
        d_[i] = d;
        ++len_;
    }
    Delta total() const {
        Delta s;
        for (std::size_t j = 0; j < len_; ++j) {
            const Delta& d = d_[(head_ + j) % kCap];
            s.cpus = std::max(s.cpus, d.cpus);
            s.events += d.events;
            s.ms += d.ms;
        }
        return s;
    }

private:
    static constexpr std::size_t kCap = 64;
    clock::duration span_;
    std::array<clock::time_point, kCap> at_{};
    std::array<Delta, kCap> d_{};
    std::size_t head_ = 0, len_ = 0;
};

}  // namespace rockbottom::throttle
