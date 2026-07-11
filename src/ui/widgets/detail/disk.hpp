// widgets/detail/disk.hpp — the DISK drill-down body.
//
// More than btop's disk box: live system read/write sparklines, PSI I/O
// pressure (how much the machine is stalling on storage), every filesystem
// with its device, type, free space and a fullness meter, a plain-language
// callout for whichever mount is closest to full, and the top processes
// actually driving disk traffic right now.

#pragma once

#include "common.hpp"

namespace rockbottom::ui::detail {

inline std::vector<Element> disk_body(const Snapshot& s, const Ctx& cx) {
    using namespace maya; using namespace maya::dsl;
    std::vector<Element> b;

    // ── system I/O ───────────────────────────────────────────────────────────────
    // Sparks are peak-normalized — Spark clamps to [0,1]; raw B/s histories
    // would render a solid wall. Peak figures ride the section rule.
    float rpk = 1, wpk = 1;
    auto rdn = norm48(s.disk_io.read_history.data(), s.disk_io.hist_len, &rpk);
    auto wrn = norm48(s.disk_io.write_history.data(), s.disk_io.hist_len, &wpk);
    b.push_back(section("SYSTEM I/O", pal::disk_ac));
    b.push_back((h(
        text("  ▼ read") | nowrap | fgc(pal::teal) | width(10),
        Element{Spark{rdn.data(), s.disk_io.hist_len}.fill().color(pal::teal).baseline(true)} | grow(1),
        text(humanize_rate(s.disk_io.read)) | nowrap | Bold | fgc(pal::teal) | width(10) | justify(Justify::End),
        text(fmt::count(s.disk_io.read_iops) + " iops") | nowrap | fgc(pal::dim) | width(11) | justify(Justify::End),
        text("pk " + std::string(humanize_rate(ByteRate{rpk}))) | nowrap | fgc(pal::dim) | width(12) | justify(Justify::End)
    ) | gap(1)).build());
    b.push_back((h(
        text("  ▲ write") | nowrap | fgc(pal::hot) | width(10),
        Element{Spark{wrn.data(), s.disk_io.hist_len}.fill().color(pal::hot).baseline(true)} | grow(1),
        text(humanize_rate(s.disk_io.write)) | nowrap | Bold | fgc(pal::hot) | width(10) | justify(Justify::End),
        text(fmt::count(s.disk_io.write_iops) + " iops") | nowrap | fgc(pal::dim) | width(11) | justify(Justify::End),
        text("pk " + std::string(humanize_rate(ByteRate{wpk}))) | nowrap | fgc(pal::dim) | width(12) | justify(Justify::End)
    ) | gap(1)).build());
    if (s.psi.io.available) {
        b.push_back(bar("io pressure", s.psi.io.some_avg10 / 100.0, "of last 10s stalled on I/O", pal::hot, cx.wide ? 34 : 0));
        if (s.psi.io.some_avg10 > 20)
            b.push_back(verdict("▲ storage is a bottleneck — tasks are spending real time waiting on the disk", pal::hot));
    }
    b.push_back(gap_row());

    // ── filesystems ───────────────────────────────────────────────
    b.push_back(section("FILESYSTEMS", pal::disk_ac,
                        std::to_string(static_cast<int>(s.disks.size())) + " mounted"));
    {
        // Header columns MUST mirror the data-row layout below exactly, or the
        // labels drift off their values. The data row is:
        //   mount(16) · meter(grow) · pct(5) · free(9) · used/size(16) · inodes(7) · fs(10)
        // so the header carries the same 7 slots — "usage" spans the meter,
        // an empty slot sits over the pct column.
        auto col = [](const char* t) { return text(t) | nowrap | fgc(pal::dim); };
        b.push_back((h(
            col("mount") | width(16),
            col("usage") | grow(1),
            col("") | width(5) | justify(Justify::End),
            col("free") | width(9) | justify(Justify::End),
            col("used / size") | width(16) | justify(Justify::End),
            col("inodes") | width(7) | justify(Justify::End),
            col("  fs") | width(10)
        ) | gap(1) | bgc(pal::track)).build());
    }
    const DiskInfo* worst = nullptr;
    for (const DiskInfo& d : s.disks) {
        const double f = d.usage().v;
        const std::uint64_t freeb = d.total.value > d.used.value ? d.total.value - d.used.value : 0;
        if (!worst || f > worst->usage().v) worst = &d;
        // Inode pressure — the disk-full failure nobody checks for until
        // "No space left on device" with 40G free.
        std::string ino = "·";
        maya::Color ino_c = pal::faint;
        if (d.inodes_total > 0) {
            const double iused = 1.0 - static_cast<double>(d.inodes_free) /
                                          static_cast<double>(d.inodes_total);
            ino = fmt::pct(iused);
            ino_c = iused > 0.9 ? pal::crit : iused > 0.7 ? pal::hot : pal::dim;
        }
        std::string fstag = "  " + d.fstype + (d.read_only ? " ro" : "");
        b.push_back((h(
            text(fmt::clip(d.mount, 15)) | nowrap | Bold | fgc(pal::text) | width(16),
            Element{Meter{f}.fill().color(pal::disk_ac)} | grow(1),
            text(fmt::pct_pad(f)) | nowrap | Bold | fgc(load_color(f)) | width(5) | justify(Justify::End),
            text(humanize_bytes(freeb)) | nowrap | fgc(f > 0.9 ? pal::crit : pal::good) | width(9) | justify(Justify::End),
            text(humanize_bytes(d.used) + " / " + humanize_bytes(d.total)) | nowrap | fgc(pal::label) | width(16) | justify(Justify::End),
            text(ino) | nowrap | fgc(ino_c) | width(7) | justify(Justify::End),
            text(fstag) | nowrap | fgc(d.read_only ? pal::hot : mix(pal::disk_ac, pal::dim, 0.4)) | width(10)
        ) | gap(1)).build());
        // On wide terminals, show the backing device under the mount.
        if (cx.wide && !d.device.empty())
            b.push_back((text("  └ " + d.device) | nowrap | fgc(pal::faint)).build());
    }
    if (worst) {
        const double f = worst->usage().v;
        const std::uint64_t freeb = worst->total.value > worst->used.value ? worst->total.value - worst->used.value : 0;
        b.push_back(verdict(
            f > 0.95 ? "▲ " + worst->mount + " is essentially full (" + fmt::pct(f) + ") — " + humanize_bytes(freeb) + " left, clear space now"
          : f > 0.85 ? "▲ " + worst->mount + " is filling up (" + fmt::pct(f) + ") — keep an eye on it"
          :            "● fullest is " + worst->mount + " at " + fmt::pct(f) + " — plenty of room everywhere",
            f > 0.95 ? pal::crit : f > 0.85 ? pal::hot : pal::good));
    }
    b.push_back(gap_row());

    // ── top I/O processes ────────────────────────────────────────────────────
    {
        std::vector<const ProcInfo*> top;
        for (const auto& p : s.procs)
            if (p.io_read.per_sec + p.io_write.per_sec > 1) top.push_back(&p);
        std::sort(top.begin(), top.end(), [](const ProcInfo* a, const ProcInfo* b2) {
            return (a->io_read.per_sec + a->io_write.per_sec) > (b2->io_read.per_sec + b2->io_write.per_sec);
        });
        if (top.empty()) {
            b.push_back(section("BUSIEST ON DISK", pal::disk_ac));
            b.push_back(verdict("nothing is touching the disk right now", pal::dim));
        } else {
            const double top_rate = top[0]->io_read.per_sec + top[0]->io_write.per_sec;
            const int show = std::min<int>(cx.tall ? 6 : 4, static_cast<int>(top.size()));
            b.push_back(section("BUSIEST ON DISK", pal::disk_ac, "top " + std::to_string(show)));
            for (int i = 0; i < show; ++i) {
                const ProcInfo& p = *top[static_cast<std::size_t>(i)];
                const double rate = p.io_read.per_sec + p.io_write.per_sec;
                b.push_back(rank_row(i + 1, std::to_string(p.pid), std::string(fmt::clip(p.name, 22)),
                                     top_rate > 0 ? rate / top_rate : 0, pal::disk_ac,
                                     "▼ " + std::string(humanize_rate(p.io_read)), pal::teal, 12,
                                     "▲ " + std::string(humanize_rate(p.io_write)), pal::hot, 12));
            }
        }
    }

    return b;
}

}  // namespace rockbottom::ui::detail
