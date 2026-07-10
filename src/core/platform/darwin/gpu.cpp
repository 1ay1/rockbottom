// platform/darwin/gpu.cpp — IOAccelerator PerformanceStatistics telemetry.
//
// The macOS analogue of the nvidia-smi / DRM sysfs scan. Every GPU (Apple
// Silicon, AMD, or Intel) publishes an IOAccelerator node in the IORegistry
// whose "PerformanceStatistics" dict carries live counters. The key names are
// not formally documented and differ by vendor, so we probe a set of known
// aliases for each metric and take whatever the card actually reports; anything
// missing stays at its sentinel and the pane omits it — the identical
// graceful-degradation contract the Linux backend uses. There is no public,
// per-process VRAM attribution on macOS, so GpuInfo::procs stays empty.

#include "../../sampler.hpp"
#include "mach_util.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

namespace rockbottom {

namespace {

double cf_num(CFDictionaryRef d, CFStringRef key, bool& found) {
    auto v = CFDictionaryGetValue(d, key);
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) { found = false; return 0; }
    double out = 0;
    CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberDoubleType, &out);
    found = true;
    return out;
}

// Try several candidate keys; return the first that exists.
double first_num(CFDictionaryRef d, std::initializer_list<CFStringRef> keys, bool& found) {
    for (CFStringRef k : keys) {
        double v = cf_num(d, k, found);
        if (found) return v;
    }
    found = false;
    return 0;
}

std::string cf_string(CFDictionaryRef d, CFStringRef key) {
    auto v = CFDictionaryGetValue(d, key);
    if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return {};
    char buf[256] = {};
    CFStringGetCString(static_cast<CFStringRef>(v), buf, sizeof buf, kCFStringEncodingUTF8);
    return buf;
}

}  // namespace

void Sampler::sample_gpu(std::vector<GpuInfo>& gpus) {
    gpus.clear();

    io_iterator_t it = MACH_PORT_NULL;
    if (::IOServiceGetMatchingServices(kIOMainPortDefault,
            ::IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS)
        return;

    for (io_registry_entry_t accel; (accel = ::IOIteratorNext(it)); ) {
        CFMutableDictionaryRef props = nullptr;
        if (::IORegistryEntryCreateCFProperties(accel, &props, kCFAllocatorDefault,
                                                kNilOptions) == KERN_SUCCESS && props) {
            GpuInfo g;
            g.vendor = "Apple";
            g.name = cf_string(props, CFSTR("model"));
            if (g.name.empty()) g.name = cf_string(props, CFSTR("IOGLBundleName"));
            if (g.name.empty()) g.name = "GPU";

            auto stats = static_cast<CFDictionaryRef>(
                CFDictionaryGetValue(props, CFSTR("PerformanceStatistics")));
            if (stats) {
                bool f = false;
                double util = first_num(stats,
                    {CFSTR("Device Utilization %"), CFSTR("GPU Activity(%)"),
                     CFSTR("Renderer Utilization %")}, f);
                if (f) g.usage = Ratio{util / 100.0};

                double used = first_num(stats,
                    {CFSTR("In use system memory"), CFSTR("vramUsedBytes"),
                     CFSTR("gartUsedBytes")}, f);
                if (f) g.mem_used = Bytes{static_cast<std::uint64_t>(used)};

                double alloc = first_num(stats,
                    {CFSTR("Alloc system memory"), CFSTR("vramFreeBytes")}, f);
                if (f && used > 0) {
                    g.mem_total = Bytes{static_cast<std::uint64_t>(used + alloc)};
                    g.mem_usage = Ratio::of(g.mem_used, g.mem_total);
                }

                double temp = first_num(stats,
                    {CFSTR("Temperature(C)"), CFSTR("GPU Temperature")}, f);
                if (f) g.temp_c = static_cast<float>(temp);
            }
            CFRelease(props);
            gpus.push_back(std::move(g));
        }
        ::IOObjectRelease(accel);
    }
    ::IOObjectRelease(it);

    // Attach rolling util/mem history per GPU index (survives across ticks) —
    // same bookkeeping the Linux backend does so the hero graph animates.
    for (std::size_t i = 0; i < gpus.size(); ++i) {
        int idx = static_cast<int>(i);
        auto& rings = gpu_hist_[idx];
        int& len = gpu_hist_len_[idx];
        sys::push_hist(rings.first, len, static_cast<float>(gpus[i].usage.v));
        int mlen = len;
        sys::push_hist(rings.second, mlen, static_cast<float>(gpus[i].mem_usage.v));
        gpus[i].util_history = rings.first;
        gpus[i].mem_history = rings.second;
        gpus[i].hist_len = len;
        gpus[i].mem_hist_len = len;
    }
}

}  // namespace rockbottom
