// platform/darwin/gpu.cpp — IOAccelerator PerformanceStatistics telemetry.
//
// The macOS analogue of the nvidia-smi / DRM sysfs scan. Every GPU (Apple
// Silicon, AMD, or Intel) publishes an IOAccelerator node in the IORegistry
// whose "PerformanceStatistics" dict carries live counters. Verified key
// names on Apple Silicon (AGXAccelerator):
//   "Device Utilization %"      — overall GPU busy
//   "Renderer Utilization %"    — 3D/render engine busy
//   "Tiler Utilization %"       — tiler/geometry engine busy
//   "In use system memory"      — bytes the GPU currently holds
//   "Alloc system memory"       — bytes the driver has allocated
// plus "model" ("Apple M1"), "gpu-core-count", and "IOSourceVersion" (driver)
// on the accelerator node itself. Apple Silicon is UNIFIED memory — the
// honest "VRAM total" is system RAM, so we report mem_total = hw.memsize and
// flag `unified` so the pane can say so instead of inventing a fake ceiling.
// AMD/Intel eGPU/iGPU Macs publish different key aliases; we probe a set of
// known names for each metric and take whatever the card reports; anything
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
    if (!v) return {};
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
        char buf[256] = {};
        CFStringGetCString(static_cast<CFStringRef>(v), buf, sizeof buf, kCFStringEncodingUTF8);
        return buf;
    }
    // Some nodes publish "model" as CFData (NUL-terminated C string).
    if (CFGetTypeID(v) == CFDataGetTypeID()) {
        auto data = static_cast<CFDataRef>(v);
        const auto* p = CFDataGetBytePtr(data);
        auto n = static_cast<std::size_t>(CFDataGetLength(data));
        std::string s(reinterpret_cast<const char*>(p), n);
        if (auto z = s.find('\0'); z != std::string::npos) s.resize(z);
        return s;
    }
    return {};
}

int cf_int(CFDictionaryRef d, CFStringRef key) {
    bool f = false;
    double v = cf_num(d, key, f);
    return f ? static_cast<int>(v) : 0;
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
            g.name = cf_string(props, CFSTR("model"));
            if (g.name.empty()) g.name = cf_string(props, CFSTR("IOGLBundleName"));
            if (g.name.empty()) g.name = "GPU";
            // Vendor from the name when possible (an AMD eGPU on a Mac should
            // not be labelled "Apple").
            if (g.name.find("AMD") != std::string::npos ||
                g.name.find("Radeon") != std::string::npos) g.vendor = "AMD";
            else if (g.name.find("Intel") != std::string::npos) g.vendor = "Intel";
            else if (g.name.find("NVIDIA") != std::string::npos ||
                     g.name.find("GeForce") != std::string::npos) g.vendor = "NVIDIA";
            else g.vendor = "Apple";

            g.driver = cf_string(props, CFSTR("IOSourceVersion"));
            g.cores = cf_int(props, CFSTR("gpu-core-count"));
            g.unified = g.vendor == "Apple";

            auto stats = static_cast<CFDictionaryRef>(
                CFDictionaryGetValue(props, CFSTR("PerformanceStatistics")));
            if (stats) {
                bool f = false;
                double util = first_num(stats,
                    {CFSTR("Device Utilization %"), CFSTR("GPU Activity(%)"),
                     CFSTR("GPU Core Utilization")}, f);
                if (f) g.usage = Ratio{util / 100.0};

                double rend = cf_num(stats, CFSTR("Renderer Utilization %"), f);
                if (f) g.renderer_usage = Ratio{rend / 100.0};
                double tiler = cf_num(stats, CFSTR("Tiler Utilization %"), f);
                if (f) g.tiler_usage = Ratio{tiler / 100.0};

                double used = first_num(stats,
                    {CFSTR("In use system memory"), CFSTR("vramUsedBytes"),
                     CFSTR("gartUsedBytes"), CFSTR("VRAM,totalMB")}, f);
                if (f) g.mem_used = Bytes{static_cast<std::uint64_t>(used)};

                if (g.unified) {
                    // Unified memory: the honest ceiling is system RAM.
                    g.mem_total = ram_total_;
                    if (g.mem_used.value)
                        g.mem_usage = Ratio::of(g.mem_used, g.mem_total);
                } else {
                    double alloc = first_num(stats,
                        {CFSTR("vramFreeBytes"), CFSTR("Alloc system memory")}, f);
                    if (f && used > 0) {
                        g.mem_total = Bytes{static_cast<std::uint64_t>(used + alloc)};
                        g.mem_usage = Ratio::of(g.mem_used, g.mem_total);
                    }
                }

                // GPU recovery count — nonzero means the GPU hung and the
                // driver reset it. Ride the pstate string (free-text slot).
                double recov = cf_num(stats, CFSTR("recoveryCount"), f);
                if (f && recov > 0)
                    g.pstate = std::to_string(static_cast<int>(recov)) + " resets";

                double temp = first_num(stats,
                    {CFSTR("Temperature(C)"), CFSTR("GPU Temperature")}, f);
                if (f) g.temp_c = static_cast<float>(temp);

                double fan = first_num(stats,
                    {CFSTR("Fan Speed(%)"), CFSTR("Fan Speed(RPM)")}, f);
                if (f) g.fan_pct = static_cast<int>(fan);

                double pwr = first_num(stats,
                    {CFSTR("Total Power(W)"), CFSTR("GPU Power(W)")}, f);
                if (f) g.power_w = pwr;

                double clk = first_num(stats,
                    {CFSTR("Core Clock(MHz)"), CFSTR("Clock(MHz)")}, f);
                if (f) g.core_clock = Hertz{static_cast<std::uint64_t>(clk) * 1000000ull};
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
        int mlen = len;   // BEFORE the util push bumps len — rings stay in step
        sys::push_hist(rings.first, len, static_cast<float>(gpus[i].usage.v));
        sys::push_hist(rings.second, mlen, static_cast<float>(gpus[i].mem_usage.v));
        gpus[i].util_history = rings.first;
        gpus[i].mem_history = rings.second;
        gpus[i].hist_len = len;
        gpus[i].mem_hist_len = len;
    }
}

}  // namespace rockbottom
