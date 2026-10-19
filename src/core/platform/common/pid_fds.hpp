// platform/common/pid_fds.hpp — /proc/<pid>/<file> fds kept open across ticks.
//
// The process scan reads /proc/N/stat (and /proc/N/io) for every process every
// tick. Keeping those fds open and re-reading with pread at offset 0 drops the
// open/close pair. An fd is bound to the process it was opened for: once that
// process is gone a read fails with ESRCH, so a reused pid can't hand us the
// old process's data; we just reopen.
//
// Fds are capped by a budget taken from RLIMIT_NOFILE. Past it a file is
// opened and closed per read, same as before.

#pragma once

#include "sys_util.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace rockbottom::procfs {

class PidFds {
public:
    enum Leaf : std::uint8_t { Stat, Io, kLeaves };

    explicit PidFds(std::size_t budget = default_budget()) : budget_(budget) {}
    PidFds(const PidFds&) = delete;
    PidFds& operator=(const PidFds&) = delete;
    ~PidFds() { clear(); }

    // Half the soft fd limit, minus headroom for everything else, capped.
    static std::size_t default_budget() {
        rlimit rl{};
        if (::getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur == RLIM_INFINITY)
            return 4096;
        const std::size_t soft = static_cast<std::size_t>(rl.rlim_cur);
        return soft > 512 ? std::min<std::size_t>((soft - 256) / 2, 32768) : 0;
    }

    // Read /proc/<pid_name>/<leaf> into out. False if it can't be read.
    bool read(int pid, const char* pid_name, Leaf leaf, std::string& out) {
        auto it = map_.find(pid);
        if (it != map_.end() && it->second[leaf] >= 0) {
            int& fd = it->second[leaf];
            const int r = pread_into(fd, out);
            if (r > 0) return true;
            // ESRCH (gone, maybe reused), or too big for the buffer: reopen.
            close_fd(fd);
            if (r == 0) return slurp(pid_name, leaf, out);
        }
        build_path(pid_name, leaf);
        if (open_ >= budget_) return sys::slurp_into(path_.c_str(), out);
        const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) { out.clear(); return false; }
        ++open_;
        if (it == map_.end()) it = map_.emplace(pid, Fds{-1, -1}).first;
        it->second[leaf] = fd;
        const int r = pread_into(it->second[leaf], out);
        if (r > 0) return true;
        close_fd(it->second[leaf]);
        return r == 0 && slurp(pid_name, leaf, out);
    }

    // Close fds for pids not in `alive` (anything with .contains(pid)).
    template <class Set>
    void retain(const Set& alive) {
        for (auto it = map_.begin(); it != map_.end(); ) {
            if (alive.contains(it->first)) { ++it; continue; }
            for (int& fd : it->second) close_fd(fd);
            it = map_.erase(it);
        }
    }

    void clear() {
        for (auto& [pid, fds] : map_) for (int& fd : fds) close_fd(fd);
        map_.clear();
    }

    std::size_t open_count() const { return open_; }

private:
    using Fds = std::array<int, kLeaves>;
    static constexpr std::size_t kBuf = 4096;   // stat and io are well under this

    // 1 = read ok, 0 = didn't fit in kBuf, -1 = error.
    static int pread_into(int fd, std::string& out) {
        ssize_t n = -1;
        out.resize_and_overwrite(kBuf, [&](char* p, std::size_t cap) {
            do { n = ::pread(fd, p, cap, 0); } while (n < 0 && errno == EINTR);
            return n > 0 ? static_cast<std::size_t>(n) : 0;
        });
        if (n <= 0) return -1;
        return static_cast<std::size_t>(n) < kBuf ? 1 : 0;
    }

    bool slurp(const char* pid_name, Leaf leaf, std::string& out) {
        build_path(pid_name, leaf);
        return sys::slurp_into(path_.c_str(), out);
    }

    void build_path(const char* pid_name, Leaf leaf) {
        path_.assign("/proc/");
        path_ += pid_name;
        path_ += leaf == Stat ? "/stat" : "/io";
    }

    void close_fd(int& fd) {
        if (fd < 0) return;
        ::close(fd);
        fd = -1;
        --open_;
    }

    std::unordered_map<int, Fds> map_;
    std::string path_;
    std::size_t open_ = 0, budget_;
};

}  // namespace rockbottom::procfs
