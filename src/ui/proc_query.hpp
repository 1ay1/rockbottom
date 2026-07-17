// ui/proc_query.hpp — the process filter query language.
//
// A single `proc_matches(p, query)` predicate powers both the flat filter and
// the flow-tree's ancestor-keeping match set, so the two views always agree on
// what a query selects. The language is deliberately small, glanceable, and
// case-insensitive — the kind of thing you type without thinking:
//
//   chrome            name OR pid substring (the everyday case)
//   user:root         owner is root (substring on the user name)
//   state:R  state:running   run state (letter or the word)
//   port:443          bound/listening port (exact number OR substring)
//   cpu:>50           cpu% comparator  (>, <, >=, <=, =)
//   mem:>500m         resident memory comparator (K/M/G suffixes)
//   !helper           NEGATE — exclude rows whose name/pid contains "helper"
//   !user:root        negate any field term
//
// Multiple space-separated terms are AND-combined ("chrome cpu:>10" = chrome
// rows over 10% cpu). It's not regex — substring + field prefixes + numeric
// comparators cover the real needs without the <regex> weight or foot-guns.

#pragma once

#include "../core/metrics.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace rockbottom::ui {

namespace query_detail {

inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline bool contains_ci(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    return lower(hay).find(lower(needle)) != std::string::npos;
}

// Parse a byte size like "500m", "2g", "1024" → bytes. Returns 0 on garbage.
inline std::uint64_t parse_bytes(const std::string& s) {
    if (s.empty()) return 0;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return 0;
    if (v < 0) return 0;   // negative → u64 cast is UB; treat as garbage
    double mult = 1;
    if (*end) {
        switch (std::tolower(static_cast<unsigned char>(*end))) {
            case 'k': mult = 1024.0; break;
            case 'm': mult = 1024.0 * 1024; break;
            case 'g': mult = 1024.0 * 1024 * 1024; break;
            case 't': mult = 1024.0 * 1024 * 1024 * 1024; break;
            default: break;
        }
    }
    return static_cast<std::uint64_t>(v * mult);
}

// A numeric comparator term like ">50" / "<=10" / "=0". Returns whether `val`
// satisfies it; a bare number means "greater-or-equal" (the useful default:
// "cpu:20" reads as "at least 20%").
inline bool num_cmp(double val, const std::string& spec, bool bytes = false) {
    if (spec.empty()) return true;
    std::size_t i = 0;
    char op = '\0';
    if (spec[0] == '>' || spec[0] == '<' || spec[0] == '=') {
        op = spec[0]; ++i;
        if (i < spec.size() && spec[i] == '=') { op = (op == '>') ? 'G' : (op == '<') ? 'L' : '='; ++i; }
    }
    std::string num = spec.substr(i);
    double thr = bytes ? static_cast<double>(parse_bytes(num)) : std::strtod(num.c_str(), nullptr);
    switch (op) {
        case '>': return val >  thr;
        case '<': return val <  thr;
        case 'G': return val >= thr;   // >=
        case 'L': return val <= thr;   // <=
        case '=': return val == thr;
        default:  return val >= thr;   // bare number → at least
    }
}

// Run-state match: "R" matches state=='R'; the word "running" also matches R,
// "sleeping"→S, "blocked"/"disk"→D, "zombie"→Z, "stopped"→T.
inline bool state_match(char st, const std::string& spec) {
    if (spec.empty()) return true;
    const std::string s = lower(spec);
    if (s.size() == 1) return std::tolower(static_cast<unsigned char>(st)) == s[0];
    char want = s == "running" ? 'R'
              : s == "sleeping" ? 'S'
              : (s == "blocked" || s == "disk" || s == "uninterruptible") ? 'D'
              : s == "zombie" ? 'Z'
              : (s == "stopped" || s == "suspended") ? 'T'
              : '?';
    return std::tolower(static_cast<unsigned char>(st)) == std::tolower(static_cast<unsigned char>(want));
}

// Evaluate ONE term (already stripped of a leading '!') against a process.
inline bool eval_term(const ProcInfo& p, const std::string& term) {
    auto colon = term.find(':');
    if (colon != std::string::npos) {
        std::string field = lower(term.substr(0, colon));
        std::string arg   = term.substr(colon + 1);
        if (field == "user" || field == "u")  return contains_ci(p.user, arg);
        if (field == "state" || field == "s") return state_match(p.state, arg);
        if (field == "port" || field == "p") {
            for (auto pt : p.ports)
                if (std::to_string(pt).find(arg) != std::string::npos) return true;
            return false;
        }
        if (field == "cpu")  return num_cmp(p.cpu, arg);
        if (field == "mem" || field == "rss")
            return num_cmp(static_cast<double>(p.rss.value), arg, /*bytes=*/true);
        if (field == "pid")  return std::to_string(p.pid).find(arg) != std::string::npos;
        if (field == "name" || field == "n") return contains_ci(p.name, arg);
        if (field == "cmd" || field == "args") return contains_ci(p.cmd, arg);
        // Unknown field → treat the whole token as a plain substring so a stray
        // colon (e.g. a path) still does something sensible.
    }
    // Plain term: name OR pid substring (the everyday case).
    return contains_ci(p.name, term) ||
           std::to_string(p.pid).find(term) != std::string::npos;
}

}  // namespace query_detail

// The one predicate both views share. Empty query matches everything. Terms are
// space-separated and AND-combined; a leading '!' negates a term.
[[nodiscard]] inline bool proc_matches(const ProcInfo& p, const std::string& query) {
    if (query.empty()) return true;
    std::size_t i = 0;
    const std::size_t n = query.size();
    while (i < n) {
        while (i < n && query[i] == ' ') ++i;
        if (i >= n) break;
        std::size_t start = i;
        while (i < n && query[i] != ' ') ++i;
        std::string term = query.substr(start, i - start);
        if (term.empty()) continue;
        bool negate = false;
        if (term[0] == '!') { negate = true; term = term.substr(1); }
        if (term.empty()) continue;
        bool hit = query_detail::eval_term(p, term);
        if (hit == negate) return false;   // AND: any term failing rejects the row
    }
    return true;
}

}  // namespace rockbottom::ui
