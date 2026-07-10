// sampler.hpp — The one impure boundary: reads /proc and /sys, produces a
// pure Snapshot. Owns the previous sample so it can compute deltas (CPU busy
// fraction, network rates, per-process CPU) across ticks.

#pragma once

#include "metrics.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace bottom {

enum class SortKey { Cpu, Mem, Pid, Name };

class Sampler {
public:
    Sampler();

    // Collect one snapshot. `sort` and `top_n` control the process table.
    Snapshot sample(SortKey sort, int top_n);

private:
    struct CpuTimes { std::uint64_t idle = 0, total = 0; };
    struct ProcPrev { std::uint64_t cpu_ticks = 0; };

    void   read_static();                // hostname, kernel, core count (once)
    void   sample_cpu(CpuInfo&);
    void   sample_mem(MemInfo&);
    void   sample_disks(std::vector<DiskInfo>&);
    void   sample_net(std::vector<NetIface>&, double dt);
    void   sample_procs(Snapshot&, SortKey, int top_n, double dt);
    Verdict judge(const Snapshot&) const;

    // Previous state for delta computation.
    CpuTimes                              prev_total_{};
    std::vector<CpuTimes>                 prev_cores_;
    std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> prev_net_;  // rx,tx
    std::unordered_map<int, ProcPrev>     prev_proc_;
    std::uint64_t                         prev_sys_cpu_total_ = 0;

    std::chrono::steady_clock::time_point last_time_{};
    bool                                  first_ = true;

    // Static, sampled once.
    std::string hostname_, kernel_, cpu_model_;
    int         ncpu_ = 1;
    long        clk_tck_ = 100;
    long        page_size_ = 4096;
    Bytes       ram_total_{};

    // Rolling histories keyed by identity so graphs survive re-sorting.
    std::array<float, 96> total_hist_{};
    int                   total_hist_len_ = 0;
    std::unordered_map<int, CpuCore>      core_hist_;  // by core index
    std::unordered_map<std::string, NetIface> net_hist_;
};

}  // namespace bottom
