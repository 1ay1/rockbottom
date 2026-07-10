// platform/darwin/battery.cpp — IOKit power-source (IOPS) battery state.
//
// The macOS analogue of /sys/class/power_supply/BAT*. IOPSCopyPowerSourcesInfo
// returns a blob describing every power source; we read the first internal
// battery's current-vs-max capacity (as a percentage) and charging state. On a
// desktop Mac with no battery the source list is empty and Battery stays absent
// — exactly what the corner chip expects.

#include "../../sampler.hpp"
#include "mach_util.hpp"   // pulls in the _Static_assert shim before Apple headers

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>

namespace rockbottom {

namespace {

int cf_int(CFDictionaryRef d, CFStringRef key, int fallback = -1) {
    auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(d, key));
    if (!n) return fallback;
    int v = fallback;
    CFNumberGetValue(n, kCFNumberIntType, &v);
    return v;
}

bool cf_str_is(CFDictionaryRef d, CFStringRef key, CFStringRef want) {
    auto s = static_cast<CFStringRef>(CFDictionaryGetValue(d, key));
    return s && CFStringCompare(s, want, 0) == kCFCompareEqualTo;
}

}  // namespace

void Sampler::sample_battery(Battery& b) {
    CFTypeRef blob = ::IOPSCopyPowerSourcesInfo();
    if (!blob) return;
    CFArrayRef list = ::IOPSCopyPowerSourcesList(blob);
    if (list) {
        for (CFIndex i = 0, n = CFArrayGetCount(list); i < n; ++i) {
            auto src = ::IOPSGetPowerSourceDescription(blob, CFArrayGetValueAtIndex(list, i));
            if (!src) continue;
            if (!cf_str_is(src, CFSTR(kIOPSTypeKey), CFSTR(kIOPSInternalBatteryType))) continue;

            int cur = cf_int(src, CFSTR(kIOPSCurrentCapacityKey), -1);
            int max = cf_int(src, CFSTR(kIOPSMaxCapacityKey), 100);
            if (cur < 0) continue;
            b.present = true;
            b.percent = max > 0 ? cur * 100 / max : cur;
            b.charging = cf_str_is(src, CFSTR(kIOPSPowerSourceStateKey),
                                   CFSTR(kIOPSACPowerValue));
            break;
        }
        CFRelease(list);
    }
    CFRelease(blob);
}

}  // namespace rockbottom
