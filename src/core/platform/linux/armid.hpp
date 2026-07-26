// platform/linux/armid.hpp — human CPU name from ARM implementer/part ids.
//
// x86 /proc/cpuinfo hands you "model name : AMD EPYC 9V74 80-Core Processor"
// and the job is done. arm64 has NO such line — the kernel publishes only
//
//     CPU implementer : 0x41
//     CPU part        : 0xd0c
//
// so a reader that only looks for "model name" falls through to a placeholder
// and every aarch64 machine — Graviton, Ampere, a Pi, essentially all of
// Android — renders as the literal string "CPU". That is what CI showed on the
// ubuntu-24.04-arm runner, and it is what this table fixes.
//
// The ids come from the ARM Main ID Register (MIDR_EL1): implementer is the
// vendor byte, part is a 12-bit vendor-assigned model. Both are also mirrored
// in the kernel's arch/arm64/include/asm/cputype.h. This is a lookup, not a
// heuristic — an unknown pair yields "" and the caller falls back to the board
// name rather than guessing.
//
// Pure and header-only so it can be unit-tested without an ARM host.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace rockbottom::arm {

// Parse "0x41" / "65" / " 0xd0c " into a number; -1 when unparseable.
inline int parse_id(const std::string& s) {
    if (s.empty()) return -1;
    const char* p = s.c_str();
    while (*p == ' ' || *p == '\t') ++p;
    if (!*p) return -1;
    char* end = nullptr;
    // Base 0 so "0x41" reads as hex and a bare "65" as decimal, matching how
    // different kernels/emulators format the same field.
    const long v = std::strtol(p, &end, 0);
    return end == p ? -1 : static_cast<int>(v);
}

// Vendor name for an ARM implementer id, or "" if unknown.
inline const char* implementer(int id) {
    switch (id) {
        case 0x41: return "ARM";
        case 0x42: return "Broadcom";
        case 0x43: return "Cavium";
        case 0x44: return "DEC";
        case 0x46: return "Fujitsu";
        case 0x48: return "HiSilicon";
        case 0x49: return "Infineon";
        case 0x4d: return "Motorola";
        case 0x4e: return "NVIDIA";
        case 0x50: return "APM";
        case 0x51: return "Qualcomm";
        case 0x53: return "Samsung";
        case 0x56: return "Marvell";
        case 0x61: return "Apple";
        case 0x66: return "Faraday";
        case 0x69: return "Intel";
        case 0x6d: return "Microsoft";
        case 0x70: return "Phytium";
        case 0xc0: return "Ampere";
        default:   return "";
    }
}

// Core name for an (implementer, part) pair, or "" if we don't know it.
//
// Part numbers are only unique WITHIN an implementer — 0xd0c is Neoverse N1
// for ARM but means nothing for Qualcomm — so the switch is nested. Covers the
// parts actually in service: ARM's Cortex-A/Neoverse lines (what every cloud
// arm64 instance and SBC reports), plus the big vendor cores.
inline const char* part_name(int impl, int part) {
    if (impl == 0x41) {   // ARM Ltd
        switch (part) {
            case 0xd03: return "Cortex-A53";
            case 0xd04: return "Cortex-A35";
            case 0xd05: return "Cortex-A55";
            case 0xd07: return "Cortex-A57";
            case 0xd08: return "Cortex-A72";
            case 0xd09: return "Cortex-A73";
            case 0xd0a: return "Cortex-A75";
            case 0xd0b: return "Cortex-A76";
            case 0xd0c: return "Neoverse-N1";
            case 0xd0d: return "Cortex-A77";
            case 0xd40: return "Neoverse-V1";
            case 0xd41: return "Cortex-A78";
            case 0xd44: return "Cortex-X1";
            case 0xd46: return "Cortex-A510";
            case 0xd47: return "Cortex-A710";
            case 0xd48: return "Cortex-X2";
            case 0xd49: return "Neoverse-N2";
            case 0xd4a: return "Neoverse-E1";
            case 0xd4b: return "Cortex-A78C";
            case 0xd4d: return "Cortex-A715";
            case 0xd4e: return "Cortex-X3";
            case 0xd4f: return "Neoverse-V2";
            case 0xd80: return "Cortex-A520";
            case 0xd81: return "Cortex-A720";
            case 0xd82: return "Cortex-X4";
            case 0xd84: return "Neoverse-V3";
            case 0xd8e: return "Neoverse-N3";
            default:    return "";
        }
    }
    if (impl == 0x51) {   // Qualcomm
        switch (part) {
            case 0x800: return "Kryo-2xx-Gold";
            case 0x801: return "Kryo-2xx-Silver";
            case 0x803: return "Kryo-3xx-Silver";
            case 0x804: return "Kryo-4xx-Gold";
            case 0x805: return "Kryo-4xx-Silver";
            case 0xc00: return "Falkor";
            case 0xc01: return "Saphira";
            default:    return "";
        }
    }
    if (impl == 0x61) {   // Apple (asahi / linux on M-series)
        switch (part) {
            case 0x022: case 0x023: return "Apple M1";
            case 0x024: case 0x025: return "Apple M1 Pro";
            case 0x028: case 0x029: return "Apple M1 Max";
            case 0x032: case 0x033: return "Apple M2";
            default:                return "";
        }
    }
    if (impl == 0xc0) {   // Ampere
        switch (part) {
            case 0xac3: case 0xac4: return "Ampere-1";
            default:                return "";
        }
    }
    if (impl == 0x4e) {   // NVIDIA
        switch (part) {
            case 0x003: return "Denver";
            case 0x004: return "Carmel";
            default:    return "";
        }
    }
    if (impl == 0x43) {   // Cavium / Marvell ThunderX
        switch (part) {
            case 0x0a1: return "ThunderX";
            case 0x0af: return "ThunderX2";
            case 0x0b8: return "OcteonTX2";
            default:    return "";
        }
    }
    return "";
}

// "0x41" + "0xd0c" -> "ARM Neoverse-N1". Degrades gracefully: a known vendor
// with an unknown part still yields the vendor plus the raw part id, which is
// strictly more useful in a bug report than "CPU". Returns "" only when even
// the implementer is unknown, letting the caller try the board name instead.
inline std::string model_name(const std::string& impl_s, const std::string& part_s) {
    const int impl = parse_id(impl_s);
    const int part = parse_id(part_s);
    if (impl < 0) return "";

    const char* vendor = implementer(impl);
    const char* core   = part >= 0 ? part_name(impl, part) : "";

    if (*core) {
        // Apple's names already carry the vendor ("Apple M1"); don't stutter.
        const std::string c = core;
        if (c.rfind(vendor, 0) == 0) return c;
        return *vendor ? std::string(vendor) + " " + c : c;
    }
    if (!*vendor) return "";

    if (part >= 0) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%x", part);
        return std::string(vendor) + " (part " + buf + ")";
    }
    return vendor;
}

}  // namespace rockbottom::arm
