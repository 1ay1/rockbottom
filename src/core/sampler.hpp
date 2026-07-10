// sampler.hpp — The metrics orchestrator.
//
// `Sampler` owns the cross-tick state needed to compute deltas (CPU busy
// fractions, network rates, per-process CPU) and the rolling history rings.
// Its per-domain collection methods are DEFINED IN SEPARATE TRANSLATION UNITS
// under core/collectors/ — cpu.cpp, mem.cpp, disk.cpp, net.cpp, proc.cpp — and
// the health verdict in core/verdict.cpp. This header is the shared contract
// between them, so each collector compiles independently.

#pragma once

#include "metrics.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bottom {

enum class SortKey { Cpu, Mem, Pid, Name };

class Sampler {
public:
    Sampler();

    // Collect one snapshot. `sort` and `top_n` shape the process table.
    Snapshot sample(SortKey sort, int top_n);

    // Static machine facts, populated once in the constructor.
    int         ncpu() const { return ncpu_; }
    Bytes       ram_total() const { return ram_total_; }

private:
    struct CpuTimes { std::uint64_t idle = 0, total = 0; };
    struct ProcPrev { std::uint64_t cpu_ticks = 0; };

    // Collectors (each lives in its own .cpp under collectors/).
    void    read_static();
    void    sample_cpu(CpuInfo&);
    void    sample_mem(MemInfo&);
    void    sample_disks(std::vector<DiskInfo>&);
    void    sample_net(std::vector<NetIface>&, double dt);
    void    sample_procs(Snapshot&, SortKey, int top_n, double dt);
    Verdict judge(const Snapshot&) const;

    // ── Cross-tick delta state ──
    CpuTimes                              prev_total_{};
    std::vector<CpuTimes>                 prev_cores_;
    std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> prev_net_;  // rx,tx
    std::unordered_map<int, ProcPrev>     prev_proc_;

    std::chrono::steady_clock::time_point last_time_{};
    bool                                  first_ = true;

    // ── Static facts (once) ──
    std::string hostname_, kernel_, cpu_model_;
    int         ncpu_ = 1;
    long        clk_tck_ = 100;
    long        page_size_ = 4096;
    Bytes       ram_total_{};

    // ── Identity-keyed history so graphs survive re-sorting ──
    std::array<float, 96>                     total_hist_{};
    int                                       total_hist_len_ = 0;
    std::unordered_map<int, CpuCore>          core_hist_;   // by core index
    std::unordered_map<std::string, NetIface> net_hist_;    // by iface name
};

}  // namespace bottom
