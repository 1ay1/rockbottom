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
#include <atomic>
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

    // The per-proc /proc/<pid>/status (ctxt switches) and /proc/<pid>/fd
    // (open-fd count) reads are the two most expensive syscalls in the tick
    // and feed ONLY the process detail pane. The UI sets the pid it's
    // inspecting (0 = none) so the bulk scan reads those two files for that
    // ONE process instead of all ~400 every tick. Atomic: set from the UI
    // thread, read from the sampler thread.
    void set_detail_pid(int pid) { detail_pid_.store(pid, std::memory_order_relaxed); }

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
    void    sample_wireless(Wireless&);
    Verdict judge(const Snapshot&) const;

    // ── Cross-tick delta state ──
    CpuTimes                              prev_total_{};
    std::uint64_t                         prev_iowait_ = 0;
    std::vector<CpuTimes>                 prev_cores_;
    // Android/Termux degraded mode: under the SELinux untrusted_app sandbox
    // the GLOBAL /proc nodes (/proc/stat, /proc/loadavg, /proc/uptime) are
    // unreadable, but per-process /proc/<pid>/stat for our own uid is not.
    // When sample_cpu can't read /proc/stat it sets cpu_stat_ok_=false and
    // sample_procs then synthesizes aggregate CPU + load from the sum of the
    // visible processes' CPU deltas, so the flagship pane isn't stuck at 0%.
    bool                                  cpu_stat_ok_ = true;   // /proc/stat readable this tick
    bool                                  loadavg_ok_ = true;    // /proc/loadavg readable
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
    std::unordered_map<int, std::pair<std::uint64_t, std::string>> cmd_cache_;  // pid -> (starttime, argv); starttime guards pid reuse
    std::unordered_map<int, std::pair<std::uint64_t, unsigned>> puid_cache_;  // pid -> (starttime, uid); skips per-tick stat()
    std::unordered_map<unsigned, std::string> uid_cache_;  // uid -> user name (getpwuid is slow)

    std::chrono::steady_clock::time_point last_time_{};
    bool                                  first_ = true;
    std::atomic<int>                      detail_pid_{0};    // proc pane target (0 = none)

    // ── Wall-clock throttles for SLOW-CHANGING collectors ──
    // These metrics (disk capacity, hardware temps, battery, PSI) move on the
    // order of seconds, not milliseconds. Re-running their syscalls/subprocess
    // every tick is pure waste — worst of all at fast refresh rates. Instead we
    // refresh each on its own wall-clock cadence and hand back a CACHED copy in
    // between, so CPU per tick stays low and, crucially, INDEPENDENT of the
    // refresh interval. Timestamps are steady_clock; a zero/`first_` forces the
    // first run.
    std::chrono::steady_clock::time_point disks_at_{}, sensors_at_{},
                                          battery_at_{}, psi_at_{}, ports_at_{},
                                          wireless_at_{};
    std::vector<DiskInfo>                 disks_cache_;
    std::vector<Sensor>                   sensors_cache_;
    Battery                               battery_cache_{};
    Psi                                   psi_cache_{};
    Wireless                              wireless_cache_{};

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
