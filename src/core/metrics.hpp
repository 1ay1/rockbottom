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

namespace rockbottom {

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
    int                  perf_cores = 0;  // performance cores (0 = homogeneous/unknown)
    int                  eff_cores = 0;   // efficiency cores
    Ratio                total{};      // aggregate busy fraction
    Ratio                user{};       // busy fraction spent in user code
    Ratio                system{};     // busy fraction spent in the kernel
    Ratio                iowait{};     // fraction of time cores sat in iowait
    std::array<double, 3> loadavg{};   // 1 / 5 / 15 minute load averages
    std::vector<CpuCore> cores;
    std::array<float, 96> total_history{};
    int                  total_hist_len = 0;
    float                temp_c = 0;    // package temperature, 0 if unavailable
};

struct MemInfo {
    Bytes total{}, used{}, available{}, cached{}, buffers{};
    Bytes wired{};        // kernel-pinned pages, can never be paged out
    Bytes compressed{};   // pages held by the memory compressor (mac) / zram
    Bytes app{};          // anonymous pages apps actually allocated
    Bytes swap_total{}, swap_used{};
    ByteRate swap_in{}, swap_out{};    // live paging activity (vmstat) — the
                                       // difference between "swap is parked"
                                       // and "the machine is thrashing"
    ByteRate page_in{}, page_out{};    // file-backed pagein/pageout traffic
    double   faults_ps = 0;            // page faults per second (all kinds)
    std::array<float, 120> usage_history{};   // usage fraction ring (leak trend)
    int hist_len = 0;
    Ratio usage() const { return Ratio::of(used, total); }
    Ratio swap_usage() const { return Ratio::of(swap_used, swap_total); }
};

struct DiskInfo {
    std::string mount, device, fstype;
    Bytes       total{}, used{};
    std::uint64_t inodes_total = 0, inodes_free = 0;   // 0 = not reported
    bool        read_only = false;
    Ratio       usage() const { return Ratio::of(used, total); }
};

struct NetIface {
    std::string name;
    std::string mac;                    // link-layer address, if known
    std::string ip4;                    // first IPv4 address, if any
    int         mtu = 0;
    Bytes       rx_total{}, tx_total{};
    ByteRate    rx{}, tx{};
    double      rx_pps = 0, tx_pps = 0;            // packets per second
    std::uint64_t rx_packets = 0, tx_packets = 0;  // lifetime packet counts
    std::uint64_t rx_errs = 0, tx_errs = 0;        // lifetime error counts
    std::uint64_t drops = 0;                       // lifetime dropped inbound
    std::array<float, 48> rx_history{}, tx_history{};
    int         hist_len = 0;
    bool        up = false;
};

// One active network socket, attributed to its owning process. The connection
// table (nethogs / lsof -i / ss territory) answers "who is talking to whom":
// local and remote endpoints, TCP state, and the pid/name that owns it.
struct Connection {
    std::string proto;      // "tcp" / "tcp6" / "udp"
    std::string laddr;      // local  ip:port
    std::string raddr;      // remote ip:port ("*" for listeners / unbound udp)
    std::string state;      // LISTEN / ESTABLISHED / TIME_WAIT / … ("" for udp)
    int         pid = 0;    // owning process, 0 if unknown
    std::string pname;      // its name, for the table
};

// One process row. `cpu` is a Ratio of *one core* (so it can exceed 1.0 across
// cores, matching top's per-core percentage convention when multiplied out).
struct ProcInfo {
    int         pid = 0;
    int         ppid = 0;       // parent pid, 0 if unknown
    std::string name, user, cmd;
    double      cpu = 0;        // % of a single core (0..100*ncores)
    Bytes       rss{};          // resident memory
    Bytes       virt{};         // virtual address-space size (htop VIRT)
    Bytes       footprint{};    // phys footprint (mac) — the honest "memory" figure
    Ratio       mem_share{};    // rss / total ram
    char        state = '?';    // R S D Z T …
    int         threads = 0;
    int         prio = 0;       // scheduling priority (platform units)
    int         nice = 0;
    int         fds = -1;       // open file descriptors, -1 unknown
    std::uint64_t start_sec = 0;   // epoch seconds the process started, 0 unknown
    std::uint64_t cpu_ms = 0;      // cumulative CPU time, milliseconds
    double      faults_ps = 0;     // page faults / second this interval
    double      csw_ps = 0;        // context switches / second this interval
    std::uint64_t pageins = 0;     // lifetime blocking pageins (disk stalls)
    ByteRate    io_read{}, io_write{};   // block-device I/O rate (/proc/pid/io)
    std::vector<std::uint16_t> ports;   // bound TCP/UDP ports (sorted, deduped)
    std::array<float, 48> cpu_history{};   // rolling per-process cpu% ring (0..1, peak-normalized at render)
    int         hist_len = 0;               // valid samples in cpu_history
};

// Aggregate health verdict — the heart of the "what's going on?" answer.
enum class Health { Calm, Busy, Stressed, Critical };

struct Verdict {
    Health      level = Health::Calm;
    std::string headline;   // one plain-language sentence
    std::string detail;     // supporting context (the culprit, usually)
};

// PSI pressure-stall info (/proc/pressure/*): the kernel's own answer to
// "what are tasks waiting on?". avg10 = % of the last 10s stalled.
struct PsiEntry {
    double some_avg10 = 0;   // ≥1 task stalled
    double full_avg10 = 0;   // ALL non-idle tasks stalled (not for cpu)
    bool   available = false;
};

struct Psi {
    PsiEntry cpu, mem, io;
};

// System-wide block-device I/O rates from /proc/diskstats deltas.
struct DiskIO {
    ByteRate read{}, write{};
    double   read_iops = 0, write_iops = 0;   // read/write OPERATIONS per second
    std::array<float, 48> read_history{}, write_history{};
    int hist_len = 0;
};

struct Battery {
    bool  present = false;
    int   percent = 0;
    bool  charging = false;
    float temp_c = 0.0f;    // battery temperature in Celsius; 0 if unknown
    // Rich fields — populated where the source exposes them (Termux:API does;
    // most Linux sysfs does too). Sentinels mean "unknown", panes omit them.
    double current_ma = 0.0;    // instantaneous current; <0 discharging, >0 charging
    int    cycles = -1;         // charge cycle count, -1 if unknown
    std::string health;         // "GOOD" / "OVERHEAT" / "DEAD" / …, empty if unknown
    std::string plug;           // "AC" / "USB" / "WIRELESS" / "" (unplugged)
    std::string tech;           // "Li-ion", …
};

// Wireless connectivity (WiFi + cellular), sourced on Android/Termux from the
// Termux:API wifi/telephony helpers. On desktop Linux these stay empty (the
// net pane already shows interface rates); the fields exist so the UI can
// surface signal strength / link speed / operator when a phone reports them.
// Any helper that a given Termux build doesn't implement just leaves its slice
// blank — `wifi_present` / `cell_present` gate the display.
struct Wireless {
    bool        wifi_present = false;
    std::string ssid;           // connected network name
    int         wifi_rssi = 0;  // dBm, 0 if unknown (closer to 0 = stronger)
    int         link_mbps = 0;  // negotiated link speed, Mbps
    int         wifi_freq = 0;  // MHz (2400/5000 band hint)
    std::string ip;             // IPv4 on the wifi link

    bool        cell_present = false;
    std::string operator_name; // carrier
    std::string net_type;      // "LTE" / "NR" / "HSPA" / …
    std::string data_state;    // "connected" / "disconnected"
};

// A process holding GPU memory / doing GPU work (from nvidia-smi compute- and
// graphics-apps, nvidia-smi pmon, or the DRM fdinfo scan). `mem` is bytes of
// VRAM the process has mapped; `sm` is its share of GPU compute (0..1) where
// the backend can attribute it per-process.
struct GpuProc {
    int         pid = 0;
    std::string name;
    Bytes       mem{};
    Ratio       sm{};        // per-process GPU/compute busy 0..1, 0 if unknown
    Ratio       enc{};       // per-process encoder busy 0..1, 0 if unknown
    Ratio       dec{};       // per-process decoder busy 0..1, 0 if unknown
    char        type = '?';  // 'C' compute · 'G' graphics · 'B' both · '?' unknown
    bool        has_util = false;  // true when sm/enc/dec are real (pmon/fdinfo)
};

// One GPU. Fields that a given vendor can't report stay at their sentinel
// (usage/temp = 0, freq = 0, power = 0) and the pane just omits them. `history`
// is the rolling utilisation ring for the hero graph.
struct GpuInfo {
    std::string           name = "GPU";
    std::string           vendor;        // "NVIDIA" / "AMD" / "Intel" / "Apple"
    std::string           driver;        // driver version, if known
    int                   cores = 0;      // GPU core count (Apple) / SM count, 0 unknown
    bool                  unified = false;// unified memory (Apple Silicon) — VRAM is RAM
    Ratio                 usage{};        // core/SM busy fraction 0..1
    Ratio                 renderer_usage{}; // 3D/render engine busy (Apple)
    Ratio                 tiler_usage{};    // tiler / geometry engine busy (Apple)
    Ratio                 mem_usage{};    // vram used / total 0..1
    Bytes                 mem_used{}, mem_total{};
    float                 temp_c = 0;     // 0 if unavailable
    double                power_w = 0;    // current draw, 0 if unavailable
    double                power_limit_w = 0;
    Hertz                 core_clock{};   // current core/SM clock
    Hertz                 mem_clock{};
    int                   fan_pct = -1;   // -1 if unavailable
    Ratio                 enc_usage{};    // encoder busy (NVENC)
    Ratio                 dec_usage{};    // decoder busy (NVDEC)
    std::string           pstate;         // perf state (P0..P8), if known
    std::array<float, 96> util_history{};
    int                   hist_len = 0;
    std::array<float, 96> mem_history{};
    int                   mem_hist_len = 0;
    std::vector<GpuProc>  procs;          // top VRAM consumers
};

// A hardware temperature sensor (or fan). Populated from Linux /sys/class/hwmon
// (coretemp, nvme, drivetemp, acpitz, chipset, battery, …); empty where the
// platform has no clean public API (macOS CPU/NVMe temps need private SMC).
struct Sensor {
    std::string label;      // "Package", "nvme0", "Core 0", "battery", …
    std::string zone;       // grouping: "cpu" / "nvme" / "drive" / "acpi" / …
    float       temp_c = 0; // current temperature, °C
    float       high_c = 0; // manufacturer high/target threshold, 0 if unknown
    float       crit_c = 0; // critical threshold, 0 if unknown
};

struct Snapshot {
    std::string           hostname, kernel;
    std::uint64_t         uptime_sec = 0;
    int                   proc_count = 0, thread_count = 0, running = 0;
    int                   zombies = 0, dstate = 0;   // Z and D state counts
    CpuInfo               cpu;
    MemInfo               mem;
    std::vector<DiskInfo> disks;
    DiskIO                disk_io;
    std::vector<NetIface> nets;
    std::vector<Connection> connections;   // active sockets + owning pids
    std::vector<GpuInfo>  gpus;
    std::vector<Sensor>   sensors;   // hwmon temps (Linux); empty on macOS
    std::vector<ProcInfo> procs;   // sorted by the active key (full list)
    Psi                   psi;
    Battery               battery;
    Wireless              wireless;   // WiFi + cellular (Termux/Android); empty on desktop
    Verdict               verdict;
};

}  // namespace rockbottom
