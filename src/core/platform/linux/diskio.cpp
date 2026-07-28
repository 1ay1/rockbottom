// collectors/diskio.cpp — /proc/diskstats: whole-system read/write rates.
//
// Sums sectors read/written across physical block devices (skipping
// partitions and virtual devices), converts deltas to bytes/sec.

#include "../../sampler.hpp"
#include "procfs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace rockbottom {

using namespace procfs;

void Sampler::sample_disk_io(DiskIO& io, double dt) {
    // Whole-file slurp + pointer-walk. The old path built a std::istringstream
    // PER LINE every tick (locale sentry + heap buffer each time) — pure waste
    // that scaled with the refresh rate. This parses in place, zero allocation.
    std::string ds = slurp("/proc/diskstats");
    std::uint64_t rd_sectors = 0, wr_sectors = 0, rd_ops = 0, wr_ops = 0;

    const char* p = ds.c_str();
    const char* end = p + ds.size();
    while (p < end) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
        const char* le = nl ? nl : end;

        auto skip_ws = [&] { while (p < le && (*p == ' ' || *p == '\t')) ++p; };
        auto next_u64 = [&]() -> std::uint64_t {
            skip_ws();
            char* e = nullptr;
            std::uint64_t v = std::strtoull(p, &e, 10);
            p = (e > p) ? e : le;
            return v;
        };

        (void)next_u64();  // major
        (void)next_u64();  // minor
        skip_ws();
        const char* ns = p;
        while (p < le && *p != ' ' && *p != '\t') ++p;
        std::size_t nlen = static_cast<std::size_t>(p - ns);
        std::uint64_t f[11] = {};
        for (auto& v : f) v = next_u64();
        // f[0] = reads completed, f[2] = sectors read, f[4] = writes completed,
        // f[6] = sectors written (Documentation/iostats).

        auto starts = [&](const char* pre) {
            std::size_t l = std::strlen(pre);
            return nlen >= l && std::memcmp(ns, pre, l) == 0;
        };
        // Whole devices only: skip partitions and virtual devices. The naive
        // "name ends in a digit" test wrongly classifies whole devices whose
        // names are inherently numbered — mmcblk0 (every phone/SBC/Chromebook),
        // md0, mtdblock0, nbd0 — which zeroed DISK I/O on those machines.
        // Digit-suffixed device families mark partitions with a 'p' separator
        // (mmcblk0p1, nvme0n1p2, md0p1); plain sd/vd/hd partitions are sdaN.
        if (starts("loop") || starts("ram") || starts("zram") ||
            starts("dm-") || starts("sr")) { p = le < end ? le + 1 : end; continue; }
        bool partition = false;
        if (starts("nvme") || starts("mmcblk") || starts("md") ||
            starts("mtdblock") || starts("nbd")) {
            // Partition iff a 'p' appears AFTER the leading letters (device
            // stems like nvme0n1 / mmcblk0 contain no 'p' past the prefix).
            std::size_t i0 = 0;
            while (i0 < nlen && !std::isdigit(static_cast<unsigned char>(ns[i0]))) ++i0;
            partition = std::memchr(ns + i0, 'p', nlen - i0) != nullptr;
        } else if (nlen) {
            partition = std::isdigit(static_cast<unsigned char>(ns[nlen - 1])) != 0;
        }
        if (!partition) {
            rd_sectors += f[2];
            wr_sectors += f[6];
            rd_ops += f[0];
            wr_ops += f[4];
        }

        p = le < end ? le + 1 : end;
    }

    constexpr std::uint64_t kSector = 512;
    std::uint64_t dr = rd_sectors > prev_io_read_  ? rd_sectors - prev_io_read_  : 0;
    std::uint64_t dw = wr_sectors > prev_io_write_ ? wr_sectors - prev_io_write_ : 0;
    io.read  = first_ ? ByteRate{0} : rate(Bytes{dr * kSector}, dt);
    io.write = first_ ? ByteRate{0} : rate(Bytes{dw * kSector}, dt);
    std::uint64_t dro = rd_ops > prev_io_rops_ ? rd_ops - prev_io_rops_ : 0;
    std::uint64_t dwo = wr_ops > prev_io_wops_ ? wr_ops - prev_io_wops_ : 0;
    io.read_iops  = (!first_ && dt > 0) ? static_cast<double>(dro) / dt : 0;
    io.write_iops = (!first_ && dt > 0) ? static_cast<double>(dwo) / dt : 0;
    prev_io_read_ = rd_sectors;
    prev_io_write_ = wr_sectors;
    prev_io_rops_ = rd_ops;
    prev_io_wops_ = wr_ops;

    push_hist2(io_read_hist_, io_write_hist_, io_hist_len_,
               static_cast<float>(io.read.per_sec),
               static_cast<float>(io.write.per_sec));

    io.read_history = io_read_hist_;
    io.write_history = io_write_hist_;
    io.hist_len = io_hist_len_;
}

// Per-physical-device I/O + service latency. Same /proc/diskstats source and
// same whole-device classification as sample_disk_io, but here we KEEP each
// device separate and read the tick counters (f3/f7/f9) the aggregate throws
// away. latency = Δticks / Δops (ms per op); busy = Δio_ticks / dt.
void Sampler::sample_drives(std::vector<DriveIO>& drives, double dt) {
    std::string ds = slurp("/proc/diskstats");
    const char* p = ds.c_str();
    const char* end = p + ds.size();
    constexpr std::uint64_t kSector = 512;

    while (p < end) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
        const char* le = nl ? nl : end;
        auto skip_ws = [&] { while (p < le && (*p == ' ' || *p == '\t')) ++p; };
        auto next_u64 = [&]() -> std::uint64_t {
            skip_ws();
            char* e = nullptr;
            std::uint64_t v = std::strtoull(p, &e, 10);
            p = (e > p) ? e : le;
            return v;
        };
        (void)next_u64();  // major
        (void)next_u64();  // minor
        skip_ws();
        const char* ns = p;
        while (p < le && *p != ' ' && *p != '\t') ++p;
        std::size_t nlen = static_cast<std::size_t>(p - ns);
        std::uint64_t f[11] = {};
        for (auto& v : f) v = next_u64();
        // f0 reads, f2 sect-rd, f3 read_ticks(ms), f4 writes, f6 sect-wr,
        // f7 write_ticks(ms), f9 io_ticks(ms device had I/O in flight).

        auto starts = [&](const char* pre) {
            std::size_t l = std::strlen(pre);
            return nlen >= l && std::memcmp(ns, pre, l) == 0;
        };
        if (starts("loop") || starts("ram") || starts("zram") ||
            starts("dm-") || starts("sr")) { p = le < end ? le + 1 : end; continue; }
        bool partition = false;
        if (starts("nvme") || starts("mmcblk") || starts("md") ||
            starts("mtdblock") || starts("nbd")) {
            std::size_t i0 = 0;
            while (i0 < nlen && !std::isdigit(static_cast<unsigned char>(ns[i0]))) ++i0;
            partition = std::memchr(ns + i0, 'p', nlen - i0) != nullptr;
        } else if (nlen) {
            partition = std::isdigit(static_cast<unsigned char>(ns[nlen - 1])) != 0;
        }
        if (partition || !nlen) { p = le < end ? le + 1 : end; continue; }

        std::string name(ns, nlen);
        DrivePrev& pv = prev_drive_[name];
        auto delta = [](std::uint64_t cur, std::uint64_t prev) {
            return cur > prev ? cur - prev : 0;
        };
        std::uint64_t d_rs = delta(f[2], pv.rd_sectors), d_ws = delta(f[6], pv.wr_sectors);
        std::uint64_t d_ro = delta(f[0], pv.rd_ops),     d_wo = delta(f[4], pv.wr_ops);
        std::uint64_t d_rt = delta(f[3], pv.rd_ticks),   d_wt = delta(f[7], pv.wr_ticks);
        std::uint64_t d_it = delta(f[9], pv.io_ticks);

        DriveIO d;
        d.name = name;
        const bool fresh = pv.rd_ops == 0 && pv.wr_ops == 0 && pv.io_ticks == 0;
        if (!first_ && !fresh && dt > 0) {
            d.read  = rate(Bytes{d_rs * kSector}, dt);
            d.write = rate(Bytes{d_ws * kSector}, dt);
            d.read_lat_ms  = d_ro ? static_cast<double>(d_rt) / static_cast<double>(d_ro) : 0;
            d.write_lat_ms = d_wo ? static_cast<double>(d_wt) / static_cast<double>(d_wo) : 0;
            // io_ticks is in ms; dt is seconds. busy = ms_busy / (dt*1000).
            d.busy = std::min(1.0, static_cast<double>(d_it) / (dt * 1000.0));
        }
        push_hist(pv.lat_hist, pv.lat_hist_len,
                  static_cast<float>(d.worst_lat_ms()));
        d.lat_history = pv.lat_hist;
        d.hist_len = pv.lat_hist_len;

        pv.rd_sectors = f[2]; pv.wr_sectors = f[6];
        pv.rd_ops = f[0];     pv.wr_ops = f[4];
        pv.rd_ticks = f[3];   pv.wr_ticks = f[7];   pv.io_ticks = f[9];

        drives.push_back(std::move(d));
        p = le < end ? le + 1 : end;
    }

    // Busiest / slowest first — that's the drive the operator cares about.
    std::sort(drives.begin(), drives.end(), [](const DriveIO& a, const DriveIO& b) {
        const double sa = a.busy + a.worst_lat_ms() / 100.0;
        const double sb = b.busy + b.worst_lat_ms() / 100.0;
        if (sa != sb) return sa > sb;
        return a.name < b.name;
    });
}

}  // namespace rockbottom
