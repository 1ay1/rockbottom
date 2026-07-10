// platform/darwin/disk.cpp — getmntinfo(3): mounted filesystems + capacity.
//
// The macOS analogue of parsing /proc/mounts + statvfs. getmntinfo hands back
// an array of `struct statfs`, each already carrying device, mountpoint,
// fstype and block counts, so no second stat() call is needed. We drop the
// synthetic/virtual filesystems a user never thinks of as "a disk" (devfs,
// autofs, the read-only system snapshot, etc.), then apply the same dedup +
// fullest-first + top-5 shaping the Linux backend uses so the UI is identical.

#include "../../sampler.hpp"
#include "mach_util.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include <sys/mount.h>
#include <sys/param.h>

namespace rockbottom {

void Sampler::sample_disks(std::vector<DiskInfo>& disks) {
    struct statfs* mnts = nullptr;
    int n = ::getmntinfo(&mnts, MNT_NOWAIT);   // pointer into a static buffer
    if (n <= 0 || !mnts) return;

    static const char* skip[] = {"devfs", "autofs", "fdesc", "procfs", "kernfs",
                                 "tmpfs", "nullfs", "volfs"};
    for (int i = 0; i < n; ++i) {
        const struct statfs& f = mnts[i];
        bool ignore = false;
        for (auto* s : skip) if (std::strcmp(f.f_fstypename, s) == 0) ignore = true;
        // Skip synthetic + read-only firmlink mounts that duplicate the root.
        if (f.f_flags & MNT_DONTBROWSE) ignore = true;
        if (ignore) continue;

        std::uint64_t bs = f.f_bsize ? f.f_bsize : 512;
        std::uint64_t total = static_cast<std::uint64_t>(f.f_blocks) * bs;
        if (total == 0) continue;

        DiskInfo d;
        d.device = f.f_mntfromname;
        d.mount  = f.f_mntonname;
        d.fstype = f.f_fstypename;
        d.total  = Bytes{total};
        d.used   = Bytes{total - static_cast<std::uint64_t>(f.f_bfree) * bs};
        d.inodes_total = f.f_files;
        d.inodes_free  = f.f_ffree;
        d.read_only    = (f.f_flags & MNT_RDONLY) != 0;
        disks.push_back(std::move(d));
    }

    // Collapse APFS volumes that share one container+capacity — keep the
    // shortest mountpoint per (device,total), matching the Linux subvolume dedup.
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

}  // namespace rockbottom
