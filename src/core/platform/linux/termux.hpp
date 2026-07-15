// platform/linux/termux.hpp — shared Termux:API plumbing.
//
// On Android/Termux the kernel sandbox (SELinux untrusted_app) hides most of
// the /sys and /proc nodes a normal Linux system monitor reads: no battery
// power_supply, no wifi sysfs, no cellular. The Termux:API app bridges that gap
// via a set of `termux-*` CLI helpers that emit JSON. This header centralises
// the three things every Termux-backed collector needs:
//
//   • termux_available()  — is this a Termux userland with the CLI present?
//     Probed ONCE and cached. Cheap access() instead of a doomed fork.
//   • termux_run(name)    — run a termux-* helper, capture stdout, or "".
//     No-ops (returns "") when the helper is missing, so callers never fork a
//     shell that would just fail.
//   • json_value / json_number — a tiny flat-JSON scraper. The termux-* tools
//     emit small, flat, well-formed objects (or arrays of them); we don't pull
//     in a JSON library for that.
//
// Every Termux helper spawns a process, so callers MUST throttle their calls
// (the sampler already refreshes battery/wifi on a slow wall-clock cadence).

#pragma once

#include "procfs.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace rockbottom::termux {

// True when the Termux:API CLI is installed. Probed once (the bin dir doesn't
// change under us) so repeated collector calls cost nothing when it's absent.
inline bool available() {
    static const bool ok = [] {
        return ::access("/data/data/com.termux/files/usr/bin/termux-battery-status",
                        X_OK) == 0;
    }();
    return ok;
}

// Confirm a SPECIFIC helper exists before spawning it — some are unimplemented
// in certain Termux builds (e.g. the Play-Store variant lacks wifi/telephony).
// Cached per-name via the caller passing its own static flag is overkill; the
// access() here is a single stat, far cheaper than the fork popen would do.
inline bool has(const char* helper) {
    if (!available()) return false;
    std::string p = "/data/data/com.termux/files/usr/bin/";
    p += helper;
    return ::access(p.c_str(), X_OK) == 0;
}

// Run a termux-* helper and slurp its stdout. Returns "" if the helper is
// missing or produced nothing. `args` is appended verbatim (already trusted,
// caller-controlled — never user input).
inline std::string run(const char* helper, const char* args = "") {
    if (!has(helper)) return {};
    std::string cmd = helper;
    if (args && *args) { cmd += ' '; cmd += args; }
    cmd += " 2>/dev/null";
    std::string out;
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return out;
    std::array<char, 1024> buf{};
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0)
        out.append(buf.data(), n);
    ::pclose(p);
    return out;
}

// Extract the value following "key" in a flat JSON object. Returns the raw
// token (a number, or the unquoted contents of a string) or "" if absent.
// `from` lets the caller scan past an earlier match (e.g. one array element).
inline std::string json_value(const std::string& j, const char* key,
                              std::size_t from = 0) {
    std::string needle = std::string("\"") + key + "\"";
    auto k = j.find(needle, from);
    if (k == std::string::npos) return {};
    auto colon = j.find(':', k + needle.size());
    if (colon == std::string::npos) return {};
    std::size_t i = colon + 1;
    while (i < j.size() && (j[i] == ' ' || j[i] == '\t')) ++i;
    if (i >= j.size()) return {};
    if (j[i] == '"') {  // string value
        auto end = j.find('"', ++i);
        if (end == std::string::npos) return {};
        return j.substr(i, end - i);
    }
    std::size_t end = i;  // bare number / literal
    while (end < j.size() && j[end] != ',' && j[end] != '}' && j[end] != '\n')
        ++end;
    return procfs::trim(j.substr(i, end - i));
}

// Convenience: the value as a double, or `dflt` when the key is absent/blank.
inline double json_number(const std::string& j, const char* key, double dflt = 0) {
    std::string v = json_value(j, key);
    return v.empty() ? dflt : std::strtod(v.c_str(), nullptr);
}

}  // namespace rockbottom::termux
