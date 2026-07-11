// sampler.hpp — The metrics orchestrator.
//
// `Sampler` owns the cross-tick state needed to compute deltas (CPU busy
// fractions, network rates, per-process CPU) and the rolling history rings.
// Its per-domain collection methods are the PLATFORM CONTRACT: this header is
// OS-agnostic, and each supported OS supplies a full set of implementations
// under core/platform/<os>/ (linux/ reads /proc + /sys; darwin/ uses
// sysctl / mach / libproc / IOKit). CMake compiles exactly one backend. The
// orchestration (sampler.cpp) and the verdict engine (verdict.cpp) stay
// platform-free, so a new OS only ever means a new platform/<os>/ directory.

#pragma once

#include "metrics.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rockbottom {

enum class SortKey { Cpu, Mem, Io, Pid, Name, Port };

// Send a signal to a pid. Returns empty string on success, error text on
// failure (permission, vanished, …). Lives here because it's the only other
// impure system boundary besides sampling.
[[nodiscard]] std::string signal_process(int pid, int sig);

// Set a process's nice value (scheduling priority, -20..19). Returns empty on
// success, error text on failure (permission to LOWER nice needs privilege).
[[nodiscard]] std::string renice_process(int pid, int nice);

class Sampler {
public:
    Sampler();

    // Collect one snapshot. `sort` and `top_n` shape the process table.
    Snapshot sample(SortKey sort, int top_n);

    // Static machine facts, populated once in the constructor.
    int         ncpu() const { return ncpu_; }
    Bytes       ram_total() const { return ram_total_; }

private:
    struct CpuTimes { std::uint64_t idle = 0, total = 0, user = 0, system = 0; };
    struct ProcPrev {
        std::uint64_t cpu_ticks = 0;
        std::uint64_t io_read = 0, io_write = 0;
        std::uint64_t faults = 0, csw = 0;
        std::array<float, 48> cpu_hist{};   // rolling cpu% (0..1) so the detail pane can graph it
        int cpu_hist_len = 0;
    };

    // Collectors (each lives in its own .cpp under platform/<os>/).
    void    read_static();
    std::uint64_t uptime_sec() const;   // seconds since boot (platform-specific)
    void    sample_cpu(CpuInfo&);
    void    sample_mem(MemInfo&);
    void    sample_mem_rates(MemInfo&, double dt);   // vmstat swap in/out
    void    sample_disks(std::vector<DiskInfo>&);
    void    sample_disk_io(DiskIO&, double dt);
    void    sample_net(std::vector<NetIface>&, double dt);
    void    sample_gpu(std::vector<GpuInfo>&);
    void    sample_sensors(std::vector<Sensor>&);
    void    sample_procs(Snapshot&, SortKey, int top_n, double dt);
    void    sample_ports();                          // fills pid_ports_
    void    sample_psi(Psi&);
    void    sample_battery(Battery&);
    Verdict judge(const Snapshot&) const;

    // ── Cross-tick delta state ──
    CpuTimes                              prev_total_{};
    std::uint64_t                         prev_iowait_ = 0;
    std::vector<CpuTimes>                 prev_cores_;
    std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> prev_net_;  // rx,tx
    std::unordered_map<std::string, std::pair<std::uint64_t, std::uint64_t>> prev_net_pkts_;  // rx,tx packets
    std::unordered_map<int, ProcPrev>     prev_proc_;
    std::uint64_t                         prev_io_read_ = 0, prev_io_write_ = 0;  // sectors
    std::uint64_t                         prev_io_rops_ = 0, prev_io_wops_ = 0;   // op counts (IOPS)
    std::uint64_t                         prev_pswpin_ = 0, prev_pswpout_ = 0;    // pages
    std::uint64_t                         prev_pgin_ = 0, prev_pgout_ = 0;        // file page io
    std::uint64_t                         prev_faults_ = 0;                       // page faults
    std::array<float, 48>                 io_read_hist_{}, io_write_hist_{};
    int                                   io_hist_len_ = 0;
    std::array<float, 120>                mem_hist_{};
    int                                   mem_hist_len_ = 0;
    std::unordered_map<int, std::vector<std::uint16_t>> pid_ports_;  // per tick
    std::vector<Connection> connections_;   // active sockets (filled by sample_ports)
    std::unordered_map<int, std::string> cmd_cache_;   // argv by pid (immutable after exec)

    std::chrono::steady_clock::time_point last_time_{};
    bool                                  first_ = true;
    unsigned                              ports_tick_ = 0;   // gates the ports scan

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
    std::unordered_map<int, std::pair<std::array<float,96>,std::array<float,96>>> gpu_hist_;  // util,mem rings by gpu index
    std::unordered_map<int, int>              gpu_hist_len_; // valid samples by gpu index
};

}  // namespace rockbottom
