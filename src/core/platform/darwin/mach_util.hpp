// platform/darwin/mach_util.hpp — macOS sysctl / mach convenience helpers.
//
// The Darwin backend leans on three kernel surfaces: sysctl (static facts +
// counters), the mach host port (host_statistics64 / host_processor_info), and
// libproc (per-process info). These small typed wrappers keep the collectors
// readable and centralise the mach_host_self() handle.

#pragma once

// ── GCC / Apple-SDK compatibility shim (MUST precede any Apple header) ────────
// Apple's kernel headers (mach/*, sys/proc_info.h, IOKit) use the C11 keyword
// `_Static_assert` at namespace scope for their struct-size locks. GCC's C++
// frontend only accepts the C++ spelling `static_assert`, so it rejects the
// SDK outright. The two are semantically identical in C++, so aliasing the
// keyword lets plain g++ parse the whole Apple SDK — which is what lets the
// macOS build use maya's blessed Homebrew-GCC toolchain instead of a fragile
// mixed Clang/libstdc++ stack. Clang accepts `_Static_assert` natively, so the
// alias is GCC-only.
#if defined(__GNUC__) && !defined(__clang__) && !defined(_Static_assert)
#  define _Static_assert static_assert
#endif

#include "../common/sys_util.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>

namespace rockbottom::mac {

// The (privilege-free) mach host port. Valid for the process lifetime.
inline mach_port_t host_port() {
    static mach_port_t p = ::mach_host_self();
    return p;
}

// sysctl by MIB name → integer of type T. std::nullopt if the key is absent.
template <class T>
inline std::optional<T> sysctl_num(const char* name) {
    T value{};
    std::size_t len = sizeof value;
    if (::sysctlbyname(name, &value, &len, nullptr, 0) != 0) return std::nullopt;
    return value;
}

// sysctl by MIB name → string. Empty on failure.
inline std::string sysctl_str(const char* name) {
    std::size_t len = 0;
    if (::sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return {};
    std::string out(len, '\0');
    if (::sysctlbyname(name, out.data(), &len, nullptr, 0) != 0) return {};
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

// sysctl by an explicit MIB array into a heap buffer (for variable-size tables
// like KERN_PROC / NET_RT_IFLIST). Returns the raw bytes, resized to fit.
inline std::vector<char> sysctl_buf(int* mib, unsigned mib_len) {
    std::size_t len = 0;
    if (::sysctl(mib, mib_len, nullptr, &len, nullptr, 0) != 0 || len == 0) return {};
    std::vector<char> buf(len);
    // The size can grow between the probe and the fetch; retry once.
    if (::sysctl(mib, mib_len, buf.data(), &len, nullptr, 0) != 0) return {};
    buf.resize(len);
    return buf;
}

}  // namespace rockbottom::mac
