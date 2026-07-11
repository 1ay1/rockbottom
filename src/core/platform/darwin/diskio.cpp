// platform/darwin/diskio.cpp — IOKit IOBlockStorageDriver byte counters.
//
// The macOS analogue of /proc/diskstats. There is no text counter file; the
// numbers live in the IORegistry. We match every IOBlockStorageDriver node
// (one per physical drive) and read its "Statistics" dict, whose "Bytes (Read)"
// and "Bytes (Write)" are cumulative byte totals since boot — so we delta them
// to bytes/sec exactly like the Linux backend deltas diskstats sectors.

#include "../../sampler.hpp"
#include "mach_util.hpp"

#include <algorithm>
#include <cstdint>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDriver.h>

namespace rockbottom {

namespace {

std::uint64_t cf_u64(CFDictionaryRef d, CFStringRef key) {
    auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(d, key));
    if (!n) return 0;
    std::int64_t v = 0;
    CFNumberGetValue(n, kCFNumberSInt64Type, &v);
    return v < 0 ? 0 : static_cast<std::uint64_t>(v);
}

}  // namespace

void Sampler::sample_disk_io(DiskIO& io, double dt) {
    std::uint64_t rd = 0, wr = 0, rops = 0, wops = 0;

    io_iterator_t it = MACH_PORT_NULL;
    if (::IOServiceGetMatchingServices(kIOMainPortDefault,
            ::IOServiceMatching(kIOBlockStorageDriverClass), &it) == KERN_SUCCESS) {
        for (io_registry_entry_t drive; (drive = ::IOIteratorNext(it)); ) {
            CFMutableDictionaryRef props = nullptr;
            if (::IORegistryEntryCreateCFProperties(drive, &props,
                    kCFAllocatorDefault, kNilOptions) == KERN_SUCCESS && props) {
                auto stats = static_cast<CFDictionaryRef>(
                    CFDictionaryGetValue(props, CFSTR(kIOBlockStorageDriverStatisticsKey)));
                if (stats) {
                    rd += cf_u64(stats, CFSTR(kIOBlockStorageDriverStatisticsBytesReadKey));
                    wr += cf_u64(stats, CFSTR(kIOBlockStorageDriverStatisticsBytesWrittenKey));
                    rops += cf_u64(stats, CFSTR(kIOBlockStorageDriverStatisticsReadsKey));
                    wops += cf_u64(stats, CFSTR(kIOBlockStorageDriverStatisticsWritesKey));
                }
                CFRelease(props);
            }
            ::IOObjectRelease(drive);
        }
        ::IOObjectRelease(it);
    }

    std::uint64_t dr = rd > prev_io_read_  ? rd - prev_io_read_  : 0;
    std::uint64_t dw = wr > prev_io_write_ ? wr - prev_io_write_ : 0;
    io.read  = first_ ? ByteRate{0} : rate(Bytes{dr}, dt);
    io.write = first_ ? ByteRate{0} : rate(Bytes{dw}, dt);
    // IOPS: operation-count deltas over dt (reuses the ops-prev members).
    std::uint64_t dro = rops > prev_io_rops_ ? rops - prev_io_rops_ : 0;
    std::uint64_t dwo = wops > prev_io_wops_ ? wops - prev_io_wops_ : 0;
    io.read_iops  = (!first_ && dt > 0) ? static_cast<double>(dro) / dt : 0;
    io.write_iops = (!first_ && dt > 0) ? static_cast<double>(dwo) / dt : 0;
    prev_io_read_ = rd;
    prev_io_write_ = wr;
    prev_io_rops_ = rops;
    prev_io_wops_ = wops;

    sys::push_hist(io_read_hist_, io_hist_len_, static_cast<float>(io.read.per_sec));
    for (int i = 1; i < io_hist_len_; ++i)
        io_write_hist_[static_cast<std::size_t>(i - 1)] = io_write_hist_[static_cast<std::size_t>(i)];
    io_write_hist_[static_cast<std::size_t>(std::min(io_hist_len_ - 1, 47))] =
        static_cast<float>(io.write.per_sec);

    io.read_history = io_read_hist_;
    io.write_history = io_write_hist_;
    io.hist_len = io_hist_len_;
}

}  // namespace rockbottom
