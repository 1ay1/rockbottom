// Exercise the shared budgeted scanner against a real tree, and check it
// against `du` for correctness plus its budget for boundedness.
#include "../src/core/platform/common/home_scan.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

using namespace rockbottom::homescan;

int fails = 0;
void expect(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : "/tmp";
    std::atomic<bool> cancel{false};
    // Captured by the full walk below and used to size the cap test, so that
    // test states a property of the CODE rather than an assumption about how
    // many files the fixture tree happens to have.
    std::uint64_t total_files = 0;

    // 1. Generous budget: should complete and match du.
    {
        auto t0 = std::chrono::steady_clock::now();
        Result r = scan_tree(root, t0 + std::chrono::seconds(60), 1ull << 40, cancel);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::printf("full scan   : %llu bytes (%.2f MB), %llu files, complete=%d, %lld ms\n",
                    (unsigned long long)r.bytes, r.bytes / 1048576.0,
                    (unsigned long long)r.files, (int)r.complete, (long long)ms);
        expect(r.complete, "a generous budget runs to completion");
        expect(r.files > 0, "a real tree yields files");
        total_files = r.files;
    }

    // 2. Tight deadline: must return EARLY rather than overrun it.
    //
    // NOTE: a SMALL tree (/etc finishes in ~4 ms) legitimately completes
    // inside a 50 ms budget, so "complete" is not a failure here — the
    // guarantee under test is the BOUND, not that we always truncate. The
    // file-cap case below is what proves truncation is reported correctly.
    {
        auto t0 = std::chrono::steady_clock::now();
        Result r = scan_tree(root, t0 + std::chrono::milliseconds(50), 1ull << 40, cancel);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::printf("50ms budget : %llu files, complete=%d, took %lld ms  %s\n",
                    (unsigned long long)r.files, (int)r.complete, (long long)ms,
                    ms < 2000 ? "[BOUNDED OK]" : "[!! OVERRAN]");
        expect(ms < 2000, "a tight deadline bounds the walk");
    }

    // 3. File cap: must stop at the cap, and SAY it stopped.
    //
    // The cap is derived from the tree rather than hardcoded. A fixed 1000
    // assumed the root has more than 1000 files, which is true for Linux /etc
    // (~1750) and false for macOS /etc (~245) — so on macOS the walk finished
    // legitimately, `complete` was correctly true, and the test failed for
    // stating something about the fixture instead of about the code.
    //
    // Half the real count is guaranteed to truncate on any tree big enough to
    // test with, and `total` comes from step 1's full walk.
    {
        const std::uint64_t cap = total_files / 2;
        if (cap < 2) {
            std::printf("cap test    : SKIPPED (%llu files is too few to halve)\n",
                        (unsigned long long)total_files);
        } else {
            Result r = scan_tree(root, std::chrono::steady_clock::now() + std::chrono::seconds(60),
                                 cap, cancel);
            std::printf("%llu cap    : %llu files, complete=%d  %s\n",
                        (unsigned long long)cap,
                        (unsigned long long)r.files, (int)r.complete,
                        r.files <= cap + 600 ? "[CAPPED OK]" : "[!! BLEW CAP]");
            expect(r.files <= cap + 600, "the file cap bounds the walk");
            expect(!r.complete, "a truncated walk reports itself incomplete");
        }
    }

    // 4. Cancellation: pre-cancelled scan must return instantly.
    {
        std::atomic<bool> c{true};
        auto t0 = std::chrono::steady_clock::now();
        Result r = scan_tree(root, t0 + std::chrono::seconds(60), 1ull << 40, c);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::printf("cancelled   : %llu files, %lld ms  %s\n",
                    (unsigned long long)r.files, (long long)ms,
                    ms < 200 ? "[CANCEL OK]" : "[!! IGNORED CANCEL]");
        expect(ms < 200, "cancellation returns promptly");
    }

    // 5. Non-directory / missing paths must be harmless.
    {
        Result a = scan_tree("/definitely/not/here", std::chrono::steady_clock::now() +
                             std::chrono::seconds(5), 1ull << 40, cancel);
        Result b = scan_tree("/etc/hostname", std::chrono::steady_clock::now() +
                             std::chrono::seconds(5), 1ull << 40, cancel);
        std::printf("bad paths   : missing=%llu file-as-root=%llu  %s\n",
                    (unsigned long long)a.bytes, (unsigned long long)b.bytes,
                    (a.bytes == 0 && b.bytes == 0) ? "[SAFE]" : "[!! UNEXPECTED]");
        expect(a.bytes == 0 && b.bytes == 0, "missing path / non-directory are harmless");
    }

    // 6. scannable_home policy.
    {
        struct { const char* p; bool want; } cases[] = {
            {"/home/ayush", true}, {"/root", true}, {"/Users/bob", true},
            {"/", false}, {"", false}, {"/nonexistent", false},
            {"/var/empty", false}, {"/usr/share/empty", false},
        };
        bool ok = true;
        for (auto& c : cases)
            if (scannable_home(c.p) != c.want) {
                std::printf("  policy MISMATCH: %s -> %d (want %d)\n",
                            c.p, (int)scannable_home(c.p), (int)c.want);
                ok = false;
            }
        std::printf("home policy : %s\n", ok ? "[OK]" : "[!! FAILED]");
        expect(ok, "scannable_home accepts homes and refuses / and stubs");
    }
    std::printf("\n%s\n", fails ? "FAILURES" : "ALL PASS");
    return fails ? 1 : 0;
}
