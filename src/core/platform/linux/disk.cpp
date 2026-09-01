// collectors/disk.cpp — /proc/mounts + statvfs, deduped by device.
//
// THE HAZARD HERE IS statvfs(2) ITSELF. On a NETWORK filesystem whose server
// has gone away (a stale NFS handle, an unplugged SMB share, a dead sshfs),
// statvfs blocks in the kernel — uninterruptibly, for as long as the mount's
// timeout allows, which for a hard NFS mount is forever. This runs on the
// sampler thread the UI waits on, so a single dead mount would freeze the
// whole monitor. The app-level watchdog would abandon the sampler after 10s,
// but a fresh Sampler walks straight back into the same mount: a permanent
// 10-second stall cycle, leaking one detached thread each time. A monitor must
// not be the thing that hangs.
//
// Two defences, in order:
//   1. Never call statvfs on a network filesystem at all. Capacity for a
//      remote share is not what a local system monitor is for, and this alone
//      removes the entire common case.
//   2. For anything else, time the call. A mount that takes longer than
//      kSlowMountMs is remembered in a POISON set and skipped on every later
//      tick for the life of the process — we pay the stall at most once, on a
//      filesystem type we did not anticipate.

#include "../../sampler.hpp"
#include "procfs.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <string>
#include <sys/statvfs.h>

namespace rockbottom {

namespace {

// Filesystems whose statvfs can block on an unreachable server. Matched
// exactly, and also by the "fuse.<transport>" prefix convention (fuse.sshfs,
// fuse.s3fs, fuse.rclone, …) which is how userspace network mounts announce
// themselves in /proc/mounts.
bool is_network_fs(const std::string& fstype) {
    static const char* kNet[] = {
        "nfs", "nfs4", "cifs", "smb3", "smbfs", "afs", "ceph", "glusterfs",
        "lustre", "ocfs2", "9p", "davfs", "sshfs", "ftpfs", "curlftpfs",
        "gfs2", "beegfs", "orangefs", "pvfs2",
    };
    for (const char* s : kNet) if (fstype == s) return true;

    // Any FUSE transport that is not explicitly local. fuse.sshfs and friends
    // are network mounts wearing a local-looking fstype.
    if (fstype.rfind("fuse.", 0) == 0) {
        static const char* kLocalFuse[] = {
            "fuse.ntfs", "fuse.ntfs-3g", "fuse.exfat", "fuse.gocryptfs",
            "fuse.encfs", "fuse.cryfs", "fuse.bindfs", "fuse.mergerfs",
            "fuse.portal",
        };
        for (const char* s : kLocalFuse) if (fstype == s) return false;
        return true;
    }
    return false;
}

// A mount that already cost us a long stall. Never probed again this run.
std::set<std::string>& poisoned_mounts() {
    static std::set<std::string> s;
    return s;
}

constexpr double kSlowMountMs = 750.0;

}  // namespace

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

        // Defence 1: never touch a network mount (see the header comment).
        // Note this sits AFTER the /dev/ prefix test, which already excludes
        // most of them — it catches the block-device-backed cluster
        // filesystems (ocfs2, gfs2) that do present a /dev/ source.
        if (is_network_fs(fstype)) continue;

        // Defence 2: a mount that blocked once is never probed again.
        if (poisoned_mounts().count(mount)) continue;

        const auto t0 = std::chrono::steady_clock::now();
        struct statvfs vfs{};
        const int rc = ::statvfs(mount.c_str(), &vfs);
        const double elapsed_ms =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();

        // Whether it succeeded or failed, if it was SLOW it is not worth the
        // sampler thread's time again. (A failure can be slow too: a hard NFS
        // mount times out eventually and then returns an error.)
        if (elapsed_ms > kSlowMountMs) poisoned_mounts().insert(mount);

        if (rc != 0) continue;
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

}  // namespace rockbottom
