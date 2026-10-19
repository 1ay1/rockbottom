// throttle.hpp — Linux thermal throttle detection, as pure logic.
//
// Temperature alone misses throttling on parts with a low trip point (issue
// #11), so we read what the kernel itself reports:
//
//   cpuN/thermal_throttle/{core,package}_throttle_{count,total_time_ms}
//       x86 only. count goes up when an event starts, total_time_ms when it
//       ends. Package counters repeat on every cpu of that package.
//   cpuN/cpufreq/{cpuinfo,scaling}_max_freq
//       scaling below cpuinfo means something capped the clock (thermal,
//       or a power profile, so it's evidence, not a verdict on its own).
//   /sys/class/thermal/cooling_deviceN with type "Processor"
//       cur_state > 0 means the thermal framework is holding the cpu down.
//
// File reads go through an injected callback so tests can fake sysfs.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace rockbottom::throttle {

using ReadFile = std::function<std::string(const std::string& path)>;

struct CpuCounters {
    bool          present = false;   // thermal_throttle/ exists for this cpu
    std::uint64_t core_count = 0, core_ms = 0;
    std::uint64_t pkg_count = 0,  pkg_ms = 0;
    std::uint64_t max_khz = 0, cap_khz = 0;   // cpuinfo_max_freq, scaling_max_freq
};

// What changed between two reads.
struct Delta {
    int           cpus = 0;     // cpus whose core or package counter moved
    std::uint64_t events = 0;   // new throttle events (max over cpus, not a sum)
    std::uint64_t ms = 0;       // throttled time that ended in the interval
};

namespace detail {
inline bool read_u64(const ReadFile& read, const std::string& p, std::uint64_t& out) {
    const std::string s = read(p);
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (end == s.c_str()) return false;
    out = v;
    return true;
}
inline std::uint64_t sub(std::uint64_t a, std::uint64_t b) { return a > b ? a - b : 0; }
}  // namespace detail

inline std::vector<CpuCounters> read_counters(int n_cpus, const ReadFile& read) {
    std::vector<CpuCounters> out(static_cast<std::size_t>(std::max(0, n_cpus)));
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::string b = "/sys/devices/system/cpu/cpu" + std::to_string(i);
        const std::string t = b + "/thermal_throttle/";
        CpuCounters& c = out[i];
        c.present = detail::read_u64(read, t + "core_throttle_count", c.core_count);
        detail::read_u64(read, t + "core_throttle_total_time_ms", c.core_ms);
        c.present |= detail::read_u64(read, t + "package_throttle_count", c.pkg_count);
        detail::read_u64(read, t + "package_throttle_total_time_ms", c.pkg_ms);
        detail::read_u64(read, b + "/cpufreq/cpuinfo_max_freq", c.max_khz);
        detail::read_u64(read, b + "/cpufreq/scaling_max_freq", c.cap_khz);
    }
    return out;
}

// Counters only grow; a drop (cpu hotplug, driver reload) counts as zero.
inline Delta diff(const std::vector<CpuCounters>& prev, const std::vector<CpuCounters>& now) {
    Delta d;
    const std::size_t n = std::min(prev.size(), now.size());
    for (std::size_t i = 0; i < n; ++i) {
        const CpuCounters& a = prev[i];
        const CpuCounters& b = now[i];
        if (!a.present || !b.present) continue;
        const std::uint64_t ce = detail::sub(b.core_count, a.core_count);
        const std::uint64_t pe = detail::sub(b.pkg_count, a.pkg_count);
        const std::uint64_t cm = detail::sub(b.core_ms, a.core_ms);
        const std::uint64_t pm = detail::sub(b.pkg_ms, a.pkg_ms);
        if (ce || pe || cm || pm) ++d.cpus;
        d.events = std::max({d.events, ce, pe});
        d.ms     = std::max({d.ms, cm, pm});
    }
    return d;
}

// Lowest scaling cap and the cpuinfo max on that same cpu, in kHz. {0,0} when
// cpufreq isn't there.
struct Cap { std::uint64_t cap_khz = 0, max_khz = 0; };
inline Cap lowest_cap(const std::vector<CpuCounters>& now) {
    Cap c;
    double worst = 2.0;
    for (const CpuCounters& x : now) {
        if (x.max_khz == 0 || x.cap_khz == 0) continue;
        const double r = static_cast<double>(x.cap_khz) / static_cast<double>(x.max_khz);
        if (r < worst) { worst = r; c = {x.cap_khz, x.max_khz}; }
    }
    return c;
}

// Paths of cooling devices whose type is "Processor". Probed once.
inline std::vector<std::string> processor_cooling(const ReadFile& read, int max_devices = 256) {
    std::vector<std::string> out;
    int misses = 0;
    for (int i = 0; i < max_devices && misses < 8; ++i) {
        const std::string b = "/sys/class/thermal/cooling_device" + std::to_string(i);
        std::string type = read(b + "/type");
        if (type.empty()) { ++misses; continue; }
        misses = 0;
        while (!type.empty() && (type.back() == '\n' || type.back() == ' ')) type.pop_back();
        if (type == "Processor") out.push_back(b);
    }
    return out;
}

// Sum of cur_state / max_state across those devices.
struct Cooling { int cur = 0, max = 0; };
inline Cooling read_cooling(const std::vector<std::string>& devs, const ReadFile& read) {
    Cooling c;
    for (const std::string& b : devs) {
        std::uint64_t cur = 0, mx = 0;
        if (!detail::read_u64(read, b + "/max_state", mx) || mx == 0) continue;
        detail::read_u64(read, b + "/cur_state", cur);
        c.cur += static_cast<int>(std::min(cur, mx));
        c.max += static_cast<int>(mx);
    }
    return c;
}

// Deltas over the last `span`, so a throttle burst stays visible for a while
// instead of flashing for one tick.
class Window {
public:
    using clock = std::chrono::steady_clock;
    explicit Window(clock::duration span = std::chrono::seconds(60)) : span_(span) {}

    void push(clock::time_point t, const Delta& d) {
        if (d.cpus || d.events || d.ms) q_.push_back({t, d});
        while (!q_.empty() && t - q_.front().first > span_) q_.pop_front();
    }
    Delta total() const {
        Delta s;
        for (const auto& [t, d] : q_) {
            s.cpus = std::max(s.cpus, d.cpus);
            s.events += d.events;
            s.ms += d.ms;
        }
        return s;
    }

private:
    clock::duration span_;
    std::deque<std::pair<clock::time_point, Delta>> q_;
};

}  // namespace rockbottom::throttle
