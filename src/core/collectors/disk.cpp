// collectors/disk.cpp — /proc/mounts + statvfs, deduped by device.

#include "../sampler.hpp"
#include "../procfs.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <sys/statvfs.h>

namespace bottom {

void Sampler::sample_disks(std::vector<DiskInfo>& disks) {
    std::ifstream mounts("/proc/mounts");
    std::string dev, mount, fstype, rest;
    while (mounts >> dev >> mount >> fstype) {
        std::getline(mounts, rest);
        // Virtual / pseudo filesystems the user doesn't think of as "disks".
        static const char* skip[] = {"proc", "sysfs", "tmpfs", "devtmpfs", "devpts",
                                     "cgroup", "cgroup2", "overlay", "squashfs", "autofs",
                                     "mqueue", "hugetlbfs", "debugfs", "tracefs", "securityfs",
                                     "pstore", "bpf", "configfs", "fusectl", "ramfs", "efivarfs"};
        bool ignore = dev.rfind("/dev/", 0) != 0;
        for (auto* s : skip) if (fstype == s) ignore = true;
        if (ignore) continue;

        struct statvfs vfs{};
        if (::statvfs(mount.c_str(), &vfs) != 0) continue;
        std::uint64_t bs = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        std::uint64_t total = vfs.f_blocks * bs;
        if (total == 0) continue;

        DiskInfo d;
        d.device = dev; d.mount = mount; d.fstype = fstype;
        d.total = Bytes{total};
        d.used  = Bytes{total - vfs.f_bfree * bs};
        disks.push_back(std::move(d));
    }

    // Collapse btrfs/bind subvolumes: many mounts share one device+capacity.
    // Keep the shortest mount path per (device,total) — that's the "real" root.
    std::sort(disks.begin(), disks.end(), [](const DiskInfo& a, const DiskInfo& b) {
        if (a.device != b.device) return a.device < b.device;
        if (a.total.value != b.total.value) return a.total.value < b.total.value;
        return a.mount.size() < b.mount.size();
    });
    disks.erase(std::unique(disks.begin(), disks.end(),
        [](const DiskInfo& a, const DiskInfo& b) {
            return a.device == b.device && a.total.value == b.total.value;
        }), disks.end());
    std::sort(disks.begin(), disks.end(),
              [](const DiskInfo& a, const DiskInfo& b) { return a.used.value > b.used.value; });
    if (disks.size() > 5) disks.resize(5);
}

}  // namespace bottom
