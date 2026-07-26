// platform/darwin/freq.hpp — live per-core CPU frequency on Apple Silicon.
//
// macOS exposes no per-core clock through any public API, so rockbottom left
// CpuCore::freq at zero and the UI's frequency column simply never appeared —
// on the one platform where the P/E split makes clock speed genuinely
// interesting. It IS recoverable, from two pieces that fit together:
//
//   1. The DVFS tables in the IORegistry (public IOKit, no entitlement) list
//      the discrete frequencies each cluster can run at:
//        voltage-states1-sram -> efficiency cluster
//        voltage-states5-sram -> performance cluster
//      Verified on an M1: E tops out at 2.064 GHz, P at 3.204 GHz, which are
//      exactly Apple's published figures.
//
//   2. IOReport publishes per-core RESIDENCY counters — how many ticks each
//      core spent in each DVFS state — under the "CPU Core Performance States"
//      subgroup, channels ECPU0..n / PCPU0..n. libIOReport is private, so it
//      is dlopen'd at runtime rather than linked: if a future macOS drops or
//      renames it, the frequency column goes back to being absent instead of
//      the app failing to launch.
//
// Current frequency is then the time-weighted mean over one sampling interval:
//
//     f = sum(freq_i * delta_residency_i) / sum(delta_residency_i)
//
// which is what a "current MHz" readout on any DVFS part actually means. The
// IDLE state is excluded from the weighting — a core that was asleep has no
// meaningful clock, and counting IDLE as 0 Hz would report a busy core as
// running slowly simply because it also idled a little.

#pragma once

#include "mach_util.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

namespace rockbottom::macfreq {

namespace detail {

inline std::string cf_str(CFStringRef s) {
    if (!s) return {};
    char buf[128] = {};
    if (!CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return {};
    return buf;
}

// Read one DVFS table out of the pmgr node. The table is an array of
// (frequency_hz, voltage) uint32 pairs; we keep the frequencies.
inline std::vector<std::uint64_t> read_dvfs(io_registry_entry_t pmgr, const char* key) {
    std::vector<std::uint64_t> out;
    CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
    if (!k) return out;
    CFTypeRef v = IORegistryEntryCreateCFProperty(pmgr, k, nullptr, 0);
    CFRelease(k);
    if (!v) return out;
    if (CFGetTypeID(v) == CFDataGetTypeID()) {
        CFDataRef d = static_cast<CFDataRef>(v);
        const std::uint8_t* p = CFDataGetBytePtr(d);
        const CFIndex n = CFDataGetLength(d);
        for (CFIndex i = 0; i + 8 <= n; i += 8) {
            std::uint32_t hz = 0;
            std::memcpy(&hz, p + i, 4);
            if (hz > 0) out.push_back(hz);
        }
    }
    CFRelease(v);
    return out;
}

}  // namespace detail

// Live per-logical-cpu frequency sampler.
//
// Holds an IOReport subscription plus the previous sample, so each call
// reports the mean clock over the interval since the last one. Constructed
// once; if anything is unavailable `ok()` stays false and the caller leaves
// frequencies at zero, exactly as before.
class Sampler {
public:
    Sampler() { init(); }
    ~Sampler() {
        if (prev_)      CFRelease(prev_);
        if (sub_chans_) CFRelease(sub_chans_);
        if (sub_)       CFRelease(static_cast<CFTypeRef>(sub_));
        if (lib_)       dlclose(lib_);
    }

    Sampler(const Sampler&)            = delete;
    Sampler& operator=(const Sampler&) = delete;

    bool ok() const { return ok_; }

    // Frequencies in Hz indexed by the channel order Apple reports: the
    // efficiency cluster first, then performance — the same order
    // host_processor_info() enumerates logical cpus in, which is what makes
    // this directly indexable by core.
    //
    // Returns empty until the second call: a residency DELTA needs two samples,
    // and reporting an absolute-counter ratio on the first tick would show the
    // machine's lifetime average clock rather than its current one.
    std::vector<std::uint64_t> sample() {
        std::vector<std::uint64_t> out;
        if (!ok_) return out;

        CFDictionaryRef now = create_samples_(sub_, sub_chans_, nullptr);
        if (!now) return out;

        if (!prev_) { prev_ = now; return out; }

        CFDictionaryRef d = create_delta_(prev_, now, nullptr);
        CFRelease(prev_);
        prev_ = now;
        if (!d) return out;

        CFArrayRef items = static_cast<CFArrayRef>(
            CFDictionaryGetValue(d, CFSTR("IOReportChannels")));
        const CFIndex n = items ? CFArrayGetCount(items) : 0;

        // Collect (channel name -> mean Hz) so ordering is ours, not Apple's.
        std::vector<std::pair<std::string, std::uint64_t>> found;
        for (CFIndex i = 0; i < n; ++i) {
            CFDictionaryRef ch = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(items, i));
            if (!ch) continue;
            const std::string sub = detail::cf_str(channel_subgroup_(ch));
            if (sub != "CPU Core Performance States") continue;

            const std::string name = detail::cf_str(channel_name_(ch));
            if (name.size() < 5) continue;
            const bool is_perf = name.rfind("PCPU", 0) == 0;
            const bool is_eff  = name.rfind("ECPU", 0) == 0;
            if (!is_perf && !is_eff) continue;

            const std::vector<std::uint64_t>& tbl = is_perf ? perf_hz_ : eff_hz_;
            if (tbl.empty()) continue;

            const int states = state_count_(ch);
            double weighted = 0, total = 0;
            for (int s = 0; s < states; ++s) {
                const std::int64_t res = state_residency_(ch, s);
                if (res <= 0) continue;
                const std::string sname = detail::cf_str(state_name_(ch, s));
                // State 0 is IDLE on every Apple part seen so far, but match by
                // NAME rather than index: a core parked in IDLE has no clock,
                // and treating it as 0 Hz would understate a busy core that
                // merely dipped idle between samples.
                if (sname == "IDLE" || sname == "OFF") continue;
                // The remaining states map 1:1, in order, onto the DVFS table.
                const std::size_t tier = static_cast<std::size_t>(s) - 1;
                if (tier >= tbl.size()) continue;
                weighted += static_cast<double>(tbl[tier]) * static_cast<double>(res);
                total    += static_cast<double>(res);
            }
            // Fully idle for the whole interval: report the cluster's lowest
            // tier rather than 0, which would render as "no data".
            const std::uint64_t hz = total > 0
                ? static_cast<std::uint64_t>(weighted / total)
                : tbl.front();
            found.emplace_back(name, hz);
        }
        CFRelease(d);

        // Efficiency cluster first, then performance — matching the mach
        // logical-cpu enumeration order this indexes into.
        auto take = [&](const char* prefix) {
            for (int idx = 0; idx < 64; ++idx) {
                const std::string want = std::string(prefix) + std::to_string(idx);
                for (const auto& [nm, hz] : found)
                    if (nm == want) { out.push_back(hz); break; }
            }
        };
        take("ECPU");
        take("PCPU");
        return out;
    }

private:
    void init() {
        // ── DVFS tables (public IOKit) ──
        io_iterator_t it{};
        if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                         IOServiceMatching("AppleARMIODevice"),
                                         &it) != KERN_SUCCESS) return;
        io_object_t obj{};
        while ((obj = IOIteratorNext(it))) {
            io_name_t nm{};
            if (IORegistryEntryGetName(obj, nm) == KERN_SUCCESS && std::strcmp(nm, "pmgr") == 0) {
                eff_hz_  = detail::read_dvfs(obj, "voltage-states1-sram");
                perf_hz_ = detail::read_dvfs(obj, "voltage-states5-sram");
            }
            IOObjectRelease(obj);
            if (!eff_hz_.empty() && !perf_hz_.empty()) break;
        }
        IOObjectRelease(it);
        if (eff_hz_.empty() && perf_hz_.empty()) return;

        // ── IOReport (private, resolved at runtime) ──
        lib_ = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY);
        if (!lib_) return;

        copy_channels_    = reinterpret_cast<CopyChannelsFn>(dlsym(lib_, "IOReportCopyChannelsInGroup"));
        create_sub_       = reinterpret_cast<CreateSubFn>(dlsym(lib_, "IOReportCreateSubscription"));
        create_samples_   = reinterpret_cast<CreateSamplesFn>(dlsym(lib_, "IOReportCreateSamples"));
        create_delta_     = reinterpret_cast<CreateDeltaFn>(dlsym(lib_, "IOReportCreateSamplesDelta"));
        state_count_      = reinterpret_cast<StateCountFn>(dlsym(lib_, "IOReportStateGetCount"));
        state_residency_  = reinterpret_cast<StateResidencyFn>(dlsym(lib_, "IOReportStateGetResidency"));
        state_name_       = reinterpret_cast<StateNameFn>(dlsym(lib_, "IOReportStateGetNameForIndex"));
        channel_name_     = reinterpret_cast<ChannelNameFn>(dlsym(lib_, "IOReportChannelGetChannelName"));
        channel_subgroup_ = reinterpret_cast<ChannelSubgroupFn>(dlsym(lib_, "IOReportChannelGetSubGroup"));

        if (!copy_channels_ || !create_sub_ || !create_samples_ || !create_delta_ ||
            !state_count_ || !state_residency_ || !state_name_ ||
            !channel_name_ || !channel_subgroup_) return;

        CFDictionaryRef chans = copy_channels_(CFSTR("CPU Stats"), nullptr, 0, 0, 0);
        if (!chans) return;
        CFMutableDictionaryRef mchans = CFDictionaryCreateMutableCopy(nullptr, 0, chans);
        CFRelease(chans);
        if (!mchans) return;

        sub_ = create_sub_(nullptr, mchans, &sub_chans_, 0, nullptr);
        CFRelease(mchans);
        if (!sub_ || !sub_chans_) return;

        ok_ = true;
    }

    using CopyChannelsFn     = CFDictionaryRef (*)(CFStringRef, CFStringRef, std::uint64_t, std::uint64_t, std::uint64_t);
    using CreateSubFn        = void* (*)(void*, CFMutableDictionaryRef, CFMutableDictionaryRef*, std::uint64_t, CFTypeRef);
    using CreateSamplesFn    = CFDictionaryRef (*)(void*, CFMutableDictionaryRef, CFTypeRef);
    using CreateDeltaFn      = CFDictionaryRef (*)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
    using StateCountFn       = int (*)(CFDictionaryRef);
    using StateResidencyFn   = std::int64_t (*)(CFDictionaryRef, int);
    using StateNameFn        = CFStringRef (*)(CFDictionaryRef, int);
    using ChannelNameFn      = CFStringRef (*)(CFDictionaryRef);
    using ChannelSubgroupFn  = CFStringRef (*)(CFDictionaryRef);

    void*                  lib_       = nullptr;
    void*                  sub_       = nullptr;
    CFMutableDictionaryRef sub_chans_ = nullptr;
    CFDictionaryRef        prev_      = nullptr;
    bool                   ok_        = false;

    std::vector<std::uint64_t> eff_hz_, perf_hz_;

    CopyChannelsFn    copy_channels_    = nullptr;
    CreateSubFn       create_sub_       = nullptr;
    CreateSamplesFn   create_samples_   = nullptr;
    CreateDeltaFn     create_delta_     = nullptr;
    StateCountFn      state_count_      = nullptr;
    StateResidencyFn  state_residency_  = nullptr;
    StateNameFn       state_name_       = nullptr;
    ChannelNameFn     channel_name_     = nullptr;
    ChannelSubgroupFn channel_subgroup_ = nullptr;
};

}  // namespace rockbottom::macfreq
