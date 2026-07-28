// collectors/ssd.cpp — NVMe write-endurance + health via the SMART / Health
// Information log page (Log Page Identifier 0x02), read with the NVMe admin
// passthrough ioctl on each /dev/nvmeN controller.
//
// This is the one collector that reaches past /proc into a privileged device
// ioctl, so it follows the tool's degrade-gracefully discipline to the letter:
// if the device can't be opened (no privilege / no NVMe) or the ioctl is
// rejected, the drive is simply skipped and the health vector stays empty.
// The UI then shows nothing — silence is correct, never a crash or an error row.
//
// The figure that matters is byte 5 of the log: `percentage_used`, the drive's
// self-reported consumption of its rated write endurance. It can exceed 100
// (the drive is past its warranty write budget), which is exactly the point at
// which the issue wants an alert.

#include "../../sampler.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

// The NVMe uapi header isn't present on every toolchain/sysroot (musl, old
// kernels). Rather than hard-depend on it, define the tiny slice we use. These
// values are ABI-stable kernel/spec constants.
#ifndef NVME_IOCTL_ADMIN_CMD
struct rb_nvme_admin_cmd {
    std::uint8_t  opcode;
    std::uint8_t  flags;
    std::uint16_t rsvd1;
    std::uint32_t nsid;
    std::uint32_t cdw2;
    std::uint32_t cdw3;
    std::uint64_t metadata;
    std::uint64_t addr;
    std::uint32_t metadata_len;
    std::uint32_t data_len;
    std::uint32_t cdw10;
    std::uint32_t cdw11;
    std::uint32_t cdw12;
    std::uint32_t cdw13;
    std::uint32_t cdw14;
    std::uint32_t cdw15;
    std::uint32_t timeout_ms;
    std::uint32_t result;
};
#define RB_NVME_ADMIN_CMD_T rb_nvme_admin_cmd
#define RB_NVME_IOCTL_ADMIN_CMD _IOWR('N', 0x41, struct rb_nvme_admin_cmd)
#else
#define RB_NVME_ADMIN_CMD_T nvme_admin_cmd
#define RB_NVME_IOCTL_ADMIN_CMD NVME_IOCTL_ADMIN_CMD
#endif

namespace rockbottom {

namespace {

constexpr std::uint8_t kAdminGetLogPage = 0x02;   // NVMe Admin: Get Log Page
constexpr std::uint8_t kLogSmartHealth  = 0x02;   // Log Page ID: SMART / Health
constexpr std::size_t  kLogLen          = 512;    // SMART log page size

// Little-endian read of a 16-bit temperature field (Kelvin) at byte offset.
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

// Read the SMART/Health log from one controller. Returns false on any failure
// (caller skips the device silently).
bool read_smart(const char* dev, SsdHealth& out) {
    int fd = ::open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    std::uint8_t log[kLogLen] = {};
    RB_NVME_ADMIN_CMD_T cmd = {};
    cmd.opcode   = kAdminGetLogPage;
    cmd.nsid     = 0xffffffffu;                       // whole controller
    cmd.addr     = reinterpret_cast<std::uintptr_t>(log);
    cmd.data_len = kLogLen;
    // CDW10: [7:0]=Log Page ID, [27:16]=NUMD low (dword count - 1).
    const std::uint32_t numd = (kLogLen / 4) - 1;
    cmd.cdw10 = static_cast<std::uint32_t>(kLogSmartHealth)
              | (static_cast<std::uint32_t>(numd & 0xffff) << 16);

    int rc = ::ioctl(fd, RB_NVME_IOCTL_ADMIN_CMD, &cmd);
    ::close(fd);
    if (rc != 0) return false;

    // SMART/Health Information log layout (NVMe base spec, Figure "SMART"):
    //   byte 0      : Critical Warning bitmask
    //   bytes 1-2   : Composite Temperature (Kelvin, LE)
    //   byte 3      : Available Spare (%)
    //   byte 4      : Available Spare Threshold (%)
    //   byte 5      : Percentage Used (endurance, may exceed 100)
    //   bytes 160-175: Media and Data Integrity Errors (128-bit LE)
    out.crit_warning = log[0];
    std::uint16_t kelvin = le16(&log[1]);
    out.temp_c      = kelvin ? static_cast<float>(kelvin) - 273.15f : 0.0f;
    out.spare_pct   = log[3];
    out.spare_thresh = log[4];
    out.pct_used    = log[5];
    // Media errors: read the low 64 bits (plenty of headroom; a drive with
    // >2^64 media errors has bigger problems than integer width).
    std::uint64_t me = 0;
    for (int i = 0; i < 8; ++i)
        me |= static_cast<std::uint64_t>(log[160 + i]) << (8 * i);
    out.media_errors = me;
    return true;
}

}  // namespace

void Sampler::sample_ssd_health(std::vector<SsdHealth>& out) {
    // Enumerate NVMe controllers: /dev/nvme0, /dev/nvme1, … (the char-device
    // controller nodes, NOT the /dev/nvme0n1 namespace block nodes — the admin
    // passthrough goes to the controller).
    DIR* d = ::opendir("/dev");
    if (!d) return;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        const char* n = e->d_name;
        if (std::strncmp(n, "nvme", 4) != 0) continue;
        // Controller node is "nvme<digits>" with no 'n' namespace suffix.
        const char* rest = n + 4;
        if (*rest == '\0') continue;
        bool controller = true;
        for (const char* q = rest; *q; ++q)
            if (*q < '0' || *q > '9') { controller = false; break; }
        if (!controller) continue;

        std::string path = std::string("/dev/") + n;
        SsdHealth h;
        h.name = n;
        if (read_smart(path.c_str(), h))
            out.push_back(std::move(h));
    }
    ::closedir(d);

    // Most-worn first — the drive nearest the end of its life leads.
    std::sort(out.begin(), out.end(), [](const SsdHealth& a, const SsdHealth& b) {
        if (a.pct_used != b.pct_used) return a.pct_used > b.pct_used;
        return a.name < b.name;
    });
}

}  // namespace rockbottom
