// collectors/diskio.cpp — /proc/diskstats: whole-system read/write rates.
//
// Sums sectors read/written across physical block devices (skipping
// partitions and virtual devices), converts deltas to bytes/sec.

#include "../sampler.hpp"
#include "../procfs.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

namespace rockbottom {

using namespace procfs;

void Sampler::sample_disk_io(DiskIO& io, double dt) {
    std::ifstream ds("/proc/diskstats");
    std::string line;
    std::uint64_t rd_sectors = 0, wr_sectors = 0;

    while (std::getline(ds, line)) {
        std::istringstream ss(line);
        int major = 0, minor = 0;
        std::string name;
        std::uint64_t f[11] = {};
        ss >> major >> minor >> name;
        for (auto& v : f) ss >> v;
        // f[2] = sectors read, f[6] = sectors written (Documentation/iostats)

        // Whole devices only: skip partitions (name ends in a digit for
        // sdaN/vdaN; nvme partitions are nvmeXnYpZ), loop/ram/zram/dm.
        if (name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0 ||
            name.rfind("zram", 0) == 0 || name.rfind("dm-", 0) == 0 ||
            name.rfind("sr", 0) == 0)
            continue;
        bool partition = false;
        if (name.rfind("nvme", 0) == 0) partition = name.find('p') != std::string::npos;
        else if (!name.empty())         partition = std::isdigit(static_cast<unsigned char>(name.back())) != 0;
        if (partition) continue;

        rd_sectors += f[2];
        wr_sectors += f[6];
    }

    constexpr std::uint64_t kSector = 512;
    std::uint64_t dr = rd_sectors > prev_io_read_  ? rd_sectors - prev_io_read_  : 0;
    std::uint64_t dw = wr_sectors > prev_io_write_ ? wr_sectors - prev_io_write_ : 0;
    io.read  = first_ ? ByteRate{0} : rate(Bytes{dr * kSector}, dt);
    io.write = first_ ? ByteRate{0} : rate(Bytes{dw * kSector}, dt);
    prev_io_read_ = rd_sectors;
    prev_io_write_ = wr_sectors;

    push_hist(io_read_hist_, io_hist_len_, static_cast<float>(io.read.per_sec));
    for (int i = 1; i < io_hist_len_; ++i)
        io_write_hist_[static_cast<std::size_t>(i - 1)] = io_write_hist_[static_cast<std::size_t>(i)];
    io_write_hist_[static_cast<std::size_t>(std::min(io_hist_len_ - 1, 47))] =
        static_cast<float>(io.write.per_sec);

    io.read_history = io_read_hist_;
    io.write_history = io_write_hist_;
    io.hist_len = io_hist_len_;
}

}  // namespace rockbottom
