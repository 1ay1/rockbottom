// platform/linux/smi_stream.hpp — one long-lived `nvidia-smi -lms` child.
//
// Spawning nvidia-smi every second costs ~12ms of cpu per run (it loads the
// driver library and inits NVML each time). In loop mode the same query costs
// ~1ms per line, so we start it once and keep the newest complete line.
//
// A reader thread blocks on the pipe and keeps the newest burst of lines (one
// per GPU, printed together) under a mutex. latest() returns it if fresh. If
// the child dies (driver reload, GPU reset) the reader restarts it after a
// backoff. stop() kills the child and joins. If rb is SIGKILLed the child
// dies of SIGPIPE on its next write.

#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace rockbottom {

class SmiStream {
public:
    using clock = std::chrono::steady_clock;

    SmiStream() = default;
    SmiStream(const SmiStream&) = delete;
    SmiStream& operator=(const SmiStream&) = delete;
    ~SmiStream() { stop(); }

    // `argv` must be an absolute exe path plus args, no trailing nullptr.
    void start(std::vector<std::string> argv) {
        if (thread_.joinable()) return;
        argv_ = std::move(argv);
        stop_.store(false);
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        stop_.store(true);
        {
            std::lock_guard lk(mu_);
            if (pid_ > 0) ::kill(pid_, SIGKILL);
        }
        if (thread_.joinable()) thread_.join();
    }

    bool running() const { return thread_.joinable(); }

    // Newest complete burst (one line per GPU), if it arrived within `max_age`.
    bool latest(std::string& out, clock::duration max_age) const {
        std::lock_guard lk(mu_);
        if (line_.empty() || clock::now() - at_ > max_age) return false;
        out = line_;
        return true;
    }

private:
    void run() {
        int backoff_ms = 1000;
        while (!stop_.load()) {
            const bool ok = run_once();
            if (stop_.load()) break;
            // Died fast: back off, up to 30s. Ran a while: retry soon.
            backoff_ms = ok ? 1000 : std::min(backoff_ms * 2, 30000);
            for (int waited = 0; waited < backoff_ms && !stop_.load(); waited += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // Returns true if the child produced output before exiting.
    bool run_once() {
        int p[2];
        if (::pipe2(p, O_CLOEXEC) != 0) return false;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, p[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addclose(&fa, p[0]);
        posix_spawn_file_actions_addclose(&fa, p[1]);
        std::vector<char*> av;
        for (auto& a : argv_) av.push_back(a.data());
        av.push_back(nullptr);

        pid_t pid = 0;
        const int rc = ::posix_spawn(&pid, av[0], &fa, nullptr, av.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        ::close(p[1]);
        if (rc != 0) { ::close(p[0]); return false; }
        {
            std::lock_guard lk(mu_);
            pid_ = pid;
        }

        bool got = false;
        std::string pending, burst;
        clock::time_point burst_at{};
        char buf[4096];
        while (!stop_.load()) {
            pollfd pfd{p[0], POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 200);
            if (pr < 0 && errno == EINTR) continue;
            if (pr < 0) break;
            const auto now = clock::now();
            // A quiet gap ends the burst: publish it.
            if (!burst.empty() && now - burst_at > std::chrono::milliseconds(150)) {
                publish(burst, burst_at);
                burst.clear();
            }
            if (pr == 0) continue;
            const ssize_t n = ::read(p[0], buf, sizeof buf);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            got = true;
            pending.append(buf, static_cast<std::size_t>(n));
            if (pending.size() > (1u << 20)) pending.clear();   // runaway output
            for (std::size_t nl; (nl = pending.find('\n')) != std::string::npos; ) {
                if (burst.empty()) burst_at = now;
                burst.append(pending, 0, nl + 1);
                pending.erase(0, nl + 1);
            }
        }
        if (!burst.empty()) publish(burst, burst_at);
        ::close(p[0]);
        ::kill(pid, SIGKILL);
        while (::waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        std::lock_guard lk(mu_);
        pid_ = 0;
        return got;
    }

    void publish(const std::string& s, clock::time_point at) {
        std::lock_guard lk(mu_);
        line_ = s;
        at_ = at;
    }

    std::vector<std::string> argv_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    pid_t pid_ = 0;
    std::string line_;
    clock::time_point at_{};
};

}  // namespace rockbottom
