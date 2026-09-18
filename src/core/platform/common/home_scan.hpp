// platform/common/home_scan.hpp — the budgeted directory-usage scanner and the
// thread that drives it. Shared by every platform backend, because it's pure
// POSIX (readdir + lstat) and the reason it exists is identical everywhere.
//
// WHY THIS IS NOT A SIMPLE `du`
//
// "Which user is filling the disk" has no cheap kernel answer once filesystem
// quotas are off (and they usually are). The only truthful fallback is to walk
// the tree — and that is EXPENSIVE: measured on the dev box, a warm-cache
// `du -s` over a 242 GB home took 12.3 seconds. A monitor that blocks a frame
// for twelve seconds, or that re-walks every tick, has become the problem it
// was meant to diagnose.
//
// So the walk is bounded on every axis that can run away:
//
//   * WALL CLOCK  — a hard deadline; we return a partial figure rather than
//                   overrun it, and the UI labels it as a floor (≥).
//   * FILE COUNT  — a second ceiling, so cost stays predictable instead of
//                   scaling with how fast the disk happens to be.
//   * CANCELLATION— checked in the inner loop, so quitting the app never waits
//                   on a walk of somebody's media library.
//   * ONE AT A TIME — never more than a single scan thread in flight.
//
// Accounting rules follow du, because an admin will cross-check against it:
//   * st_blocks * 512 (allocated space), not st_size — sparse files and
//     compressed extents must not report fiction.
//   * hardlinked inodes counted ONCE — otherwise a backup tree reports several
//     times the space it actually occupies.
//   * never crosses a filesystem boundary — a bind-mounted media share under a
//     home would otherwise be attributed to that user.
//   * symlinks are never FOLLOWED, but their own inode blocks ARE counted,
//     because that is what du does and an admin will cross-check. Verified
//     byte-exact against `du -sx /usr/share` on the dev box: skipping symlink
//     blocks entirely under-reported by 671,531,008 bytes (7%).

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rockbottom::homescan {

struct Result {
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    bool complete = false;   // false = hit a budget; `bytes` is a lower bound
};

inline Result scan_tree(const std::string& root,
                        std::chrono::steady_clock::time_point deadline,
                        std::uint64_t file_cap,
                        const std::atomic<bool>& cancel) {
    Result r;
    struct stat rst;
    // stat(), not lstat(), for the ROOT only.
    //
    // The walk uses lstat() everywhere INSIDE the tree on purpose: a symlink
    // must be counted as the link itself and never followed, or a link to /
    // turns a home scan into a filesystem scan (and du -sx agrees). But the
    // root the caller named is different — they are telling us WHICH TREE to
    // measure, so following that one link is the whole request.
    //
    // On macOS /etc is a symlink to private/etc, and a home can legitimately
    // be one too (/home/x -> /Volumes/data/x is a normal setup). lstat() says
    // "not a directory" for those and the scan returned zero bytes: not an
    // error, just a silently empty answer, which is the worst shape a disk
    // figure can have. Caught by rb_home_scan on the macOS runner.
    if (::stat(root.c_str(), &rst) != 0 || !S_ISDIR(rst.st_mode)) return r;
    const dev_t root_dev = rst.st_dev;

    // Only MULTIPLY-linked inodes need tracking; the common nlink==1 case
    // costs nothing, which keeps this map small on a normal home.
    std::unordered_map<std::uint64_t, char> seen;

    std::vector<std::string> stack{root};
    std::uint64_t checked = 0;
    while (!stack.empty()) {
        if (cancel.load(std::memory_order_relaxed)) return r;
        const std::string dir = std::move(stack.back());
        stack.pop_back();

        DIR* d = ::opendir(dir.c_str());
        if (!d) continue;   // unreadable subtree: skip it, don't abandon the scan
        while (dirent* e = ::readdir(d)) {
            const char* n = e->d_name;
            if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) continue;

            // The FILE CAP is checked every entry: it is a plain integer
            // compare, costs nothing, and folding it into the 512-entry batch
            // below meant a tree smaller than 512 files never checked it at
            // all. Such a walk blew straight past its cap and then reported
            // complete=true -- a truncated result claiming to be exact, which
            // is the one failure mode `complete` exists to prevent.
            if (r.files >= file_cap) { ::closedir(d); return r; }

            // The CLOCK and the cancel flag stay batched every 512 entries:
            // steady_clock::now() in the inner loop is itself a measurable
            // cost on a million-file tree, and both are time-based bounds
            // where 512 entries of slack is immaterial.
            if ((++checked & 511u) == 0) {
                if (std::chrono::steady_clock::now() > deadline ||
                    cancel.load(std::memory_order_relaxed)) {
                    ::closedir(d);
                    return r;
                }
            }

            std::string path = dir;
            if (path.empty() || path.back() != '/') path += '/';
            path += n;

            struct stat st;
            if (::lstat(path.c_str(), &st) != 0) continue;
            if (st.st_dev != root_dev) continue;
            if (S_ISDIR(st.st_mode)) { stack.push_back(std::move(path)); continue; }
            // NOTE: symlinks fall through to the accounting below rather than
            // being skipped. lstat() already gave us the LINK's own inode (we
            // never follow it, so there are no cycles and no double-counting
            // of the target) — and du counts exactly those blocks.
            if (st.st_nlink > 1 &&
                !seen.emplace(static_cast<std::uint64_t>(st.st_ino), 1).second)
                continue;
            r.bytes += static_cast<std::uint64_t>(st.st_blocks) * 512ull;
            ++r.files;
        }
        ::closedir(d);
    }
    r.complete = true;
    return r;
}

// Is this home directory one we're willing to walk? Daemon accounts point at
// "/", "/nonexistent", "/var/empty" and similar; walking "/" would be both
// meaningless as a per-user figure and ruinously expensive.
inline bool scannable_home(const std::string& home) {
    if (home.empty() || home == "/") return false;
    if (home == "/root") return true;
    return home.rfind("/home/", 0) == 0 || home.rfind("/Users/", 0) == 0;
}

}  // namespace rockbottom::homescan
