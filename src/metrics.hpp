// metrics.hpp — Pure value types describing one snapshot of the machine.
//
// These structs are *plain data*: no I/O, no methods with side effects. The
// sampler (sampler.hpp) produces a `Snapshot`; the UI (view) consumes one.
// Keeping the data layer pure is what lets the whole render path stay an Elm
// pure function of the model.

#pragma once

#include "units.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bottom {

// A single logical CPU with a small ring of recent load samples for the graph.
struct CpuCore {
    Ratio                usage{};      // 0..1 busy fraction over the last interval
    Hertz                freq{};       // current clock, 0 if unknown
    std::array<float, 48> history{};   // rolling load for the sparkline
    int                  hist_len = 0; // number of valid samples (grows to 48)
};

struct CpuInfo {
    std::string          model = "CPU";
    int                  logical = 0;
    Ratio                total{};      // aggregate busy fraction
    std::array<double, 3> loadavg{};   // 1 / 5 / 15 minute load averages
    std::vector<CpuCore> cores;
    std::array<float, 96> total_history{};
    int                  total_hist_len = 0;
    float                temp_c = 0;    // package temperature, 0 if unavailable
};

struct MemInfo {
    Bytes total{}, used{}, available{}, cached{}, buffers{};
    Bytes swap_total{}, swap_used{};
    Ratio usage() const { return Ratio::of(used, total); }
    Ratio swap_usage() const { return Ratio::of(swap_used, swap_total); }
};

struct DiskInfo {
    std::string mount, device, fstype;
    Bytes       total{}, used{};
    Ratio       usage() const { return Ratio::of(used, total); }
};

struct NetIface {
    std::string name;
    Bytes       rx_total{}, tx_total{};
    ByteRate    rx{}, tx{};
    std::array<float, 48> rx_history{}, tx_history{};
    int         hist_len = 0;
    bool        up = false;
};

// One process row. `cpu` is a Ratio of *one core* (so it can exceed 1.0 across
// cores, matching top's per-core percentage convention when multiplied out).
struct ProcInfo {
    int         pid = 0;
    std::string name, user, cmd;
    double      cpu = 0;        // % of a single core (0..100*ncores)
    Bytes       rss{};          // resident memory
    Ratio       mem_share{};    // rss / total ram
    char        state = '?';    // R S D Z T …
    int         threads = 0;
};

// Aggregate health verdict — the heart of the "what's going on?" answer.
enum class Health { Calm, Busy, Stressed, Critical };

struct Verdict {
    Health      level = Health::Calm;
    std::string headline;   // one plain-language sentence
    std::string detail;     // supporting context (the culprit, usually)
};

struct Snapshot {
    std::string           hostname, kernel;
    std::uint64_t         uptime_sec = 0;
    int                   proc_count = 0, thread_count = 0, running = 0;
    CpuInfo               cpu;
    MemInfo               mem;
    std::vector<DiskInfo> disks;
    std::vector<NetIface> nets;
    std::vector<ProcInfo> procs;   // sorted by the active key, top N kept
    Verdict               verdict;
};

}  // namespace bottom
