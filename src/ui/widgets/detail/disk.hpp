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

    // In ultrawide mode the pane splits into two side-by-side columns. `L`
    // collects the left (system I/O graph + I/O pressure), `R` the right
    // (per-filesystem list + busiest processes). In normal mode both point at
    // the same vector and everything stacks as before.
    std::vector<Element> single;
    std::vector<Element> left, right;
    const bool split = cx.ultrawide;
    std::vector<Element>& L = split ? left : single;
    std::vector<Element>& R = split ? right : single;

    // ── system I/O ────────────────────────────────────────────────────────────────────────
    // A single hero graph carries BOTH directions: read as the filled
    // mountain, write as an overlaid line on the same peak-normalized grid
    // (the net-pane idiom). Both series share one peak so their heights are
    // directly comparable; the y-axis reads that peak as a byte rate.
    float rpk = 1, wpk = 1;
    norm48(s.disk_io.read_history.data(), s.disk_io.hist_len, &rpk);
    norm48(s.disk_io.write_history.data(), s.disk_io.hist_len, &wpk);
    // Re-normalize both to a SHARED peak so read fill and write overlay are
    // on the same scale (norm48 peaks each series independently otherwise).
    const float shared_pk = std::max({rpk, wpk, 1.0f});
    std::array<float, 48> rds{}, wrs{};
    for (int i = 0; i < s.disk_io.hist_len && i < 48; ++i) {
        rds[static_cast<std::size_t>(i)] = s.disk_io.read_history[static_cast<std::size_t>(i)] / shared_pk;
        wrs[static_cast<std::size_t>(i)] = s.disk_io.write_history[static_cast<std::size_t>(i)] / shared_pk;
    }
    L.push_back(section("SYSTEM I/O", pal::disk_ac));
    {
        const int gh = std::max(4, cx.graph_h - 1);
        L.push_back((h(
            y_axis(gh, static_cast<double>(shared_pk), 5, /*percent=*/false),
            Element{Graph{rds.data(), s.disk_io.hist_len}.fill().rows(gh).color(pal::teal)
                        .overlay(wrs.data(), s.disk_io.hist_len, pal::hot)} | grow(1)
        ) | gap(1) | height(gh)).build());
    }
    // Live figures + peaks for each direction, keyed by the graph's colors.
    L.push_back((h(
        text("  ▼ read") | nowrap | fgc(pal::teal) | width(10),
        text(humanize_rate(s.disk_io.read)) | nowrap | Bold | fgc(pal::teal) | width(11) | justify(Justify::End),
        text(fmt::count(s.disk_io.read_iops) + " iops") | nowrap | fgc(pal::dim) | width(12) | justify(Justify::End),
        text("pk " + std::string(humanize_rate(ByteRate{rpk}))) | nowrap | fgc(pal::dim) | width(13) | justify(Justify::End)
    ) | gap(1)).build());
    L.push_back((h(
        text("  ▲ write") | nowrap | fgc(pal::hot) | width(10),
        text(humanize_rate(s.disk_io.write)) | nowrap | Bold | fgc(pal::hot) | width(11) | justify(Justify::End),
        text(fmt::count(s.disk_io.write_iops) + " iops") | nowrap | fgc(pal::dim) | width(12) | justify(Justify::End),
        text("pk " + std::string(humanize_rate(ByteRate{wpk}))) | nowrap | fgc(pal::dim) | width(13) | justify(Justify::End)
    ) | gap(1)).build());
    // Combined IOPS strip — operations/sec is the metric SSDs bottleneck on
    // long before bandwidth does, so surface it as its own honest row.
    L.push_back(kv3(
        "read iops", fmt::count(s.disk_io.read_iops) + "/s",
        s.disk_io.read_iops > 5000 ? pal::hot : pal::teal,
        "write iops", fmt::count(s.disk_io.write_iops) + "/s",
        s.disk_io.write_iops > 5000 ? pal::hot : pal::hot,
        "total iops", fmt::count(s.disk_io.read_iops + s.disk_io.write_iops) + "/s",
        pal::label));
    if (s.psi.io.available) {
        L.push_back(bar("io pressure", s.psi.io.some_avg10 / 100.0, "of last 10s stalled on I/O", pal::hot, cx.wide ? 34 : 0));
        if (s.psi.io.some_avg10 > 20)
            L.push_back(verdict("▲ storage is a bottleneck — tasks are spending real time waiting on the disk", pal::hot));
    }
    L.push_back(gap_row());

    // ── filesystems ─────────────────────────────────────
    R.push_back(section("FILESYSTEMS", pal::disk_ac,
                        std::to_string(static_cast<int>(s.disks.size())) + " mounted"));
    {
        // Header columns MUST mirror the data-row layout below exactly, or the
        // labels drift off their values. The data row is:
        //   mount(16) · meter(grow) · pct(5) · free(9) · used/size(16) · inodes(7) · fs(10)
        // so the header carries the same 7 slots — "usage" spans the meter,
        // an empty slot sits over the pct column.
        auto col = [](const char* t) { return text(t) | nowrap | fgc(pal::dim); };
        R.push_back((h(
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
        R.push_back((h(
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
            R.push_back((text("  └ " + d.device) | nowrap | fgc(pal::faint)).build());
        // Inode fullness as its own meter where the fs reports it — the
        // second way a disk fills up (millions of tiny files) that a byte
        // meter can't see. Shown on tall/wide screens so idle rows stay
        // compact; the count tail carries the raw free/total figures.
        if (cx.wide && d.inodes_total > 0) {
            const double iused = 1.0 - static_cast<double>(d.inodes_free) /
                                          static_cast<double>(d.inodes_total);
            const std::uint64_t iuse = d.inodes_total - d.inodes_free;
            R.push_back((h(
                text("    inodes") | nowrap | fgc(pal::faint) | width(14),
                Element{Meter{iused}.fill().groove(false).color(load_color(iused))} | grow(1),
                text(fmt::pct_pad(iused)) | nowrap | Bold | fgc(load_color(iused)) | width(5) | justify(Justify::End),
                text(fmt::count(static_cast<double>(iuse)) + " / " +
                     fmt::count(static_cast<double>(d.inodes_total))) | nowrap | fgc(pal::dim) | width(16) | justify(Justify::End)
            ) | gap(1)).build());
        }
    }
    if (worst) {
        const double f = worst->usage().v;
        const std::uint64_t freeb = worst->total.value > worst->used.value ? worst->total.value - worst->used.value : 0;
        R.push_back(verdict(
            f > 0.95 ? "▲ " + worst->mount + " is essentially full (" + fmt::pct(f) + ") — " + humanize_bytes(freeb) + " left, clear space now"
          : f > 0.85 ? "▲ " + worst->mount + " is filling up (" + fmt::pct(f) + ") — keep an eye on it"
          :            "● fullest is " + worst->mount + " at " + fmt::pct(f) + " — plenty of room everywhere",
            f > 0.95 ? pal::crit : f > 0.85 ? pal::hot : pal::good));
    }
    R.push_back(gap_row());

    // ── top I/O processes ────────────────────────────────────────────────────
    {
        std::vector<const ProcInfo*> top;
        for (const auto& p : s.procs)
            if (p.io_read.per_sec + p.io_write.per_sec > 1) top.push_back(&p);
        std::sort(top.begin(), top.end(), [](const ProcInfo* a, const ProcInfo* b2) {
            return (a->io_read.per_sec + a->io_write.per_sec) > (b2->io_read.per_sec + b2->io_write.per_sec);
        });
        if (top.empty()) {
            R.push_back(section("BUSIEST ON DISK", pal::disk_ac));
            R.push_back(verdict("nothing is touching the disk right now", pal::dim));
        } else {
            const double top_rate = top[0]->io_read.per_sec + top[0]->io_write.per_sec;
            const int show = std::min<int>(cx.tall ? 8 : 4, static_cast<int>(top.size()));
            R.push_back(section("BUSIEST ON DISK", pal::disk_ac, "top " + std::to_string(show)));
            for (int i = 0; i < show; ++i) {
                const ProcInfo& p = *top[static_cast<std::size_t>(i)];
                const double rate = p.io_read.per_sec + p.io_write.per_sec;
                R.push_back(rank_row(i + 1, std::to_string(p.pid), std::string(fmt::clip(p.name, 22)),
                                     top_rate > 0 ? rate / top_rate : 0, pal::disk_ac,
                                     "▼ " + std::string(humanize_rate(p.io_read)), pal::teal, 12,
                                     "▲ " + std::string(humanize_rate(p.io_write)), pal::hot, 12));
            }
        }
    }

    if (split) return two_col(std::move(left), std::move(right));
    return single;
}

}  // namespace rockbottom::ui::detail
