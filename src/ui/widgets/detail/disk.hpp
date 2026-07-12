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

// A width-aware FILESYSTEMS row (shared by the header and every mount). The
// mount name, the usage meter, and the pct always render; the trailing
// figures (free → used/size → inodes → fs) shed right-to-left as the pane
// narrows, so a thin DISK pane reads "/home ███ 27%" cleanly instead of
// crushing every column into an unreadable stub. `meter` is nullopt on the
// header row (its slot becomes the "usage" label spanning the groove).
struct FsTail { std::string text; maya::Color color; int min_w; };

inline Element build_fs_line(std::string mount, maya::Color mount_c,
                             std::optional<double> meter, std::string pct,
                             maya::Color pct_c, std::vector<FsTail> tails,
                             bool header) {
    using namespace maya;
    return Element{maya::ComponentElement{
        .render = [=](int w, int) -> Element {
            using namespace maya::dsl;
            constexpr int kMountW = 15, kPctW = 5, kMeterMin = 6, kGap = 1;
            int budget = w - kMountW - kPctW - kMeterMin - 3 * kGap;
            std::vector<const FsTail*> keep;
            for (const auto& t : tails) {
                const int need = std::max(t.min_w, static_cast<int>(string_width(t.text))) + kGap;
                if (budget >= need) { keep.push_back(&t); budget -= need; }
                else break;   // priority order: once one won't fit, drop the rest
            }
            std::vector<Element> row;
            Style mst = Style{}.with_fg(mount_c);
            if (!header) mst = mst.with_bold();
            row.push_back((text(maya::truncate_end(mount, kMountW), mst)
                           | nowrap | width(kMountW + 1)).build());
            if (meter)
                row.push_back(Element{Meter{*meter}.fill().color(pal::disk_ac)} | grow(1));
            else
                row.push_back((text("usage") | nowrap | fgc(pal::dim) | grow(1)).build());
            row.push_back((text(pct) | nowrap | Bold | fgc(pct_c) | width(kPctW) | justify(Justify::End)).build());
            for (const FsTail* t : keep)
                row.push_back((text(t->text) | nowrap | fgc(t->color)
                               | width(std::max(t->min_w, static_cast<int>(string_width(t->text))))
                               | justify(Justify::End)).build());
            Element line = (h(std::move(row)) | gap(kGap)).build();
            if (header) return (Element{std::move(line)} | bgc(pal::track)).build();
            return line;
        },
    }};
}

inline std::vector<Element> disk_body(const Snapshot& s, const Ctx& cx) {
    using namespace maya; using namespace maya::dsl;

    // In ultrawide mode the pane splits into two side-by-side columns. `L`
    // collects the left (system I/O graph + I/O pressure), `R` the right
    // (per-filesystem list + busiest processes). In normal mode both point at
    // the same vector and everything stacks as before.
    std::vector<Element> single;
    std::vector<Element> hero, left, right;
    const bool split = cx.ultrawide;
    std::vector<Element>& L = split ? left : single;
    std::vector<Element>& R = split ? right : single;
    // The SYSTEM I/O graph rides the full pane width in split mode.
    std::vector<Element>& H = split ? hero : single;

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
    H.push_back(section("SYSTEM I/O", pal::disk_ac));
    {
        const int gh = std::max(4, cx.graph_h - 1);
        H.push_back((h(
            y_axis(gh, static_cast<double>(shared_pk), 5, /*percent=*/false),
            Element{Graph{rds.data(), s.disk_io.hist_len}.fill().rows(gh).color(pal::teal)
                        .overlay(wrs.data(), s.disk_io.hist_len, pal::hot)} | grow(1)
        ) | gap(1) | height(gh)).build());
    }
    // Live figures + peaks for each direction, keyed by the graph's colors.
    // Width-aware: iops then peak shed right-to-left on a thin pane so the
    // rate stays readable instead of every column truncating to a stub.
    L.push_back(flow_row("  \xe2\x96\xbc rd", pal::teal, rds.data(), s.disk_io.hist_len, pal::teal,
        std::string(humanize_rate(s.disk_io.read)), pal::teal,
        {{fmt::count(s.disk_io.read_iops) + " iops", pal::dim, 9},
         {"pk " + std::string(humanize_rate(ByteRate{rpk})), pal::dim, 10}}));
    L.push_back(flow_row("  \xe2\x96\xb2 wr", pal::hot, wrs.data(), s.disk_io.hist_len, pal::hot,
        std::string(humanize_rate(s.disk_io.write)), pal::hot,
        {{fmt::count(s.disk_io.write_iops) + " iops", pal::dim, 9},
         {"pk " + std::string(humanize_rate(ByteRate{wpk})), pal::dim, 10}}));
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
    // The row is width-aware: mount + meter + pct always show; free, then
    // used/size, then inodes, then fs shed right-to-left as the pane narrows,
    // so at 30 cols you read "/home ██ 27%" cleanly instead of every column
    // truncating into stubs ("61G /", "btr"). Header + data share the same
    // shed logic so labels never drift off their values.
    // The inodes column only appears when at least ONE mount actually reports
    // inode stats — on filesystems that don't (many btrfs/vfat via statvfs)
    // it would otherwise be a dead column of "·" under a header that reads as
    // a promise the data never keeps.
    bool any_inodes = false;
    for (const DiskInfo& d : s.disks)
        if (d.inodes_total > 0) { any_inodes = true; break; }
    {
        // Header carries the same slots the data rows do; both funnel through
        // build_fs_line so the shed decisions match exactly.
        std::vector<FsTail> htail{{"free", pal::dim, 8}, {"used / size", pal::dim, 13}};
        if (any_inodes) htail.push_back({"inodes", pal::dim, 6});
        htail.push_back({"fs", pal::dim, 6});
        R.push_back(build_fs_line(
            /*mount=*/"mount", pal::dim, /*meter=*/std::nullopt, /*pct=*/"",
            pal::dim, htail, /*header=*/true));
    }
    const DiskInfo* worst = nullptr;
    for (const DiskInfo& d : s.disks) {
        const double f = d.usage().v;
        const std::uint64_t freeb = d.total.value > d.used.value ? d.total.value - d.used.value : 0;
        if (!worst || f > worst->usage().v) worst = &d;
        std::string ino = "\xc2\xb7";
        maya::Color ino_c = pal::faint;
        if (d.inodes_total > 0) {
            const double iused = 1.0 - static_cast<double>(d.inodes_free) /
                                          static_cast<double>(d.inodes_total);
            ino = fmt::pct(iused);
            ino_c = iused > 0.9 ? pal::crit : iused > 0.7 ? pal::hot : pal::dim;
        }
        std::string fstag = d.fstype + (d.read_only ? " ro" : "");
        std::vector<FsTail> tail{
            {std::string(humanize_bytes(freeb)), f > 0.9 ? pal::crit : pal::good, 8},
            {std::string(humanize_bytes(d.used)) + " / " + std::string(humanize_bytes(d.total)), pal::label, 13}};
        if (any_inodes) tail.push_back({ino, ino_c, 6});
        tail.push_back({fstag, d.read_only ? pal::hot : mix(pal::disk_ac, pal::dim, 0.4), 6});
        R.push_back(build_fs_line(
            maya::truncate_end(d.mount, 15), pal::text, f, fmt::pct_pad(f),
            load_color(f), tail, /*header=*/false));
        // On wide terminals, show the backing device under the mount.
        if (cx.wide && !d.device.empty())
            R.push_back((text("  \xe2\x94\x94 " + d.device) | nowrap | fgc(pal::faint)).build());
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
                R.push_back(rank_row(i + 1, std::to_string(p.pid), maya::truncate_end(p.name, 22),
                                     top_rate > 0 ? rate / top_rate : 0, pal::disk_ac,
                                     "▼ " + std::string(humanize_rate(p.io_read)), pal::teal, 12,
                                     "▲ " + std::string(humanize_rate(p.io_write)), pal::hot, 12));
            }
        }
    }

    if (split) return hero_split(std::move(hero), std::move(left), std::move(right));
    return single;
}

}  // namespace rockbottom::ui::detail
