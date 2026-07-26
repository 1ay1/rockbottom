// platform/darwin/smc.hpp — minimal AppleSMC client for die temperatures.
//
// macOS ships no /sys and no public temperature API, so rockbottom's macOS
// sensor collector used to return an empty list and every temperature in the UI
// rendered as a dash. The readings DO exist: AppleSMC is a public IOKit service
// and — verified on an M1 — opens for an ordinary uid with no entitlement, no
// root, and no private framework linked at build time.
//
// The protocol is a single structured call on the SMC user client. Each key is
// a FourCC; reading one means asking for its metadata (type + size) and then
// its bytes. We enumerate the key table by index rather than guessing names,
// because the sensor set differs per SoC generation — hardcoding an M1 list
// would silently produce nothing on an M3.
//
// Keys of interest on Apple Silicon:
//   Tp**   performance-cluster CPU die sensors
//   Te**   efficiency-cluster CPU die sensors
//   TG**   GPU        Ts**/TB**  battery/enclosure
// Several sensors exist per cluster (different points on the die); we average
// the live ones rather than pick one, because any single point is noisy and
// some read 0 when their domain is powered down.

#pragma once

#include "mach_util.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <IOKit/IOKitLib.h>

namespace rockbottom::smc {

// ── wire structs ────────────────────────────────────────────────────────────
// Layout is fixed by the kernel side; field names follow the widely-used
// reverse-engineered definitions. Only the pieces we need are meaningful.
struct Vers   { std::uint8_t major, minor, build, reserved; std::uint16_t release; };
struct PLimit { std::uint16_t version, length; std::uint32_t cpu, gpu, mem; };
struct KeyInfo { std::uint32_t size, type; std::uint8_t attributes; };

struct Param {
    std::uint32_t key;
    Vers          vers;
    PLimit        plimit;
    KeyInfo       key_info;
    std::uint8_t  result, status, data8;
    std::uint32_t data32;
    std::uint8_t  bytes[32];
};

enum : std::uint32_t { kIndexSMC = 2 };
enum : std::uint8_t  { kCmdReadBytes = 5, kCmdReadIndex = 8, kCmdReadKeyInfo = 9 };

constexpr std::uint32_t fourcc(const char* s) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[2])) << 8)  |
            static_cast<std::uint32_t>(static_cast<unsigned char>(s[3]));
}

inline std::string key_str(std::uint32_t k) {
    char b[5] = {static_cast<char>((k >> 24) & 0xff), static_cast<char>((k >> 16) & 0xff),
                 static_cast<char>((k >> 8) & 0xff),  static_cast<char>(k & 0xff), 0};
    return b;
}

// One reading: the key that produced it and its value in degrees C.
struct Reading {
    std::string key;
    float       value = 0;
};

// RAII handle on the SMC user client. Constructed once and kept — opening the
// connection is the expensive part, and the sensor collector runs on a timer.
class Connection {
public:
    Connection() {
        io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                       IOServiceMatching("AppleSMC"));
        if (!svc) return;
        if (IOServiceOpen(svc, mach_task_self(), 0, &conn_) != KERN_SUCCESS) conn_ = 0;
        IOObjectRelease(svc);
    }
    ~Connection() { if (conn_) IOServiceClose(conn_); }

    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    bool ok() const { return conn_ != 0; }

    // Every temperature key the SMC publishes, with its current value.
    //
    // The key table is enumerated ONCE and cached: it is a static property of
    // the machine, while the values are not. Re-enumerating means ~1600 extra
    // round trips per refresh, which is the difference between a sensor read
    // that's free and one you can feel.
    std::vector<Reading> temperatures() {
        std::vector<Reading> out;
        if (!conn_) return out;
        if (temp_keys_.empty()) enumerate_temp_keys();

        out.reserve(temp_keys_.size());
        for (const CachedKey& ck : temp_keys_) {
            float v = 0;
            if (read_float(ck, v) && v > -50.0f && v < 150.0f) out.push_back({ck.name, v});
        }
        return out;
    }

private:
    struct CachedKey {
        std::uint32_t key = 0;
        KeyInfo       info{};
        std::string   name;
    };

    kern_return_t call(const Param& in, Param& out) const {
        std::size_t osz = sizeof(Param);
        return IOConnectCallStructMethod(conn_, kIndexSMC, &in, sizeof(Param), &out, &osz);
    }

    bool key_info_for(std::uint32_t key, KeyInfo& out_info) const {
        Param in{}, out{};
        in.key = key;
        in.data8 = kCmdReadKeyInfo;
        if (call(in, out) != KERN_SUCCESS) return false;
        out_info = out.key_info;
        return true;
    }

    // Enumerate the key table and keep the temperature sensors we can decode.
    void enumerate_temp_keys() {
        // "#KEY" holds the number of published keys.
        KeyInfo ki{};
        if (!key_info_for(fourcc("#KEY"), ki)) return;
        Param in{}, out{};
        in.key = fourcc("#KEY");
        in.data8 = kCmdReadBytes;
        in.key_info.size = ki.size;
        if (call(in, out) != KERN_SUCCESS) return;
        const std::uint32_t total = (static_cast<std::uint32_t>(out.bytes[0]) << 24) |
                                    (static_cast<std::uint32_t>(out.bytes[1]) << 16) |
                                    (static_cast<std::uint32_t>(out.bytes[2]) << 8)  |
                                     static_cast<std::uint32_t>(out.bytes[3]);
        if (total == 0 || total > 8192) return;   // implausible: bail rather than spin

        temp_keys_.reserve(64);
        for (std::uint32_t i = 0; i < total; ++i) {
            Param q{}, r{};
            q.data8 = kCmdReadIndex;
            q.data32 = i;
            if (call(q, r) != KERN_SUCCESS) continue;

            const std::string name = key_str(r.key);
            if (name.empty() || name[0] != 'T') continue;   // temperatures only

            KeyInfo info{};
            if (!key_info_for(r.key, info)) continue;
            // Only the float form is worth carrying on Apple Silicon; the
            // legacy fixed-point 'sp78' encoding is Intel-era, and 'ioft' keys
            // on this machine all read zero.
            if (info.type != fourcc("flt ") || info.size != 4) continue;

            temp_keys_.push_back({r.key, info, name});
        }
    }

    bool read_float(const CachedKey& ck, float& out_val) const {
        Param in{}, out{};
        in.key = ck.key;
        in.data8 = kCmdReadBytes;
        in.key_info = ck.info;
        if (call(in, out) != KERN_SUCCESS) return false;
        std::memcpy(&out_val, out.bytes, sizeof(float));
        return true;
    }

    io_connect_t           conn_ = 0;
    std::vector<CachedKey> temp_keys_;
};

}  // namespace rockbottom::smc
