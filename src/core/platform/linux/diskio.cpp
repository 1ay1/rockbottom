// collectors/diskio.cpp — /proc/diskstats: whole-system read/write rates.
//
// Sums sectors read/written across physical block devices (skipping
// partitions and virtual devices), converts deltas to bytes/sec.

#include "../../sampler.hpp"
#include "procfs.hpp"

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
        // Whole devices only: skip partitions (name ends in a digit for
        // sdaN/vdaN; nvme partitions are nvmeXnYpZ), loop/ram/zram/dm.
        if (starts("loop") || starts("ram") || starts("zram") ||
            starts("dm-") || starts("sr")) { p = le < end ? le + 1 : end; continue; }
        bool partition = false;
        if (starts("nvme"))
            partition = std::memchr(ns, 'p', nlen) != nullptr;
        else if (nlen)
            partition = std::isdigit(static_cast<unsigned char>(ns[nlen - 1])) != 0;
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

}  // namespace rockbottom
