// widgets/disk_panel.hpp — filesystem card: deduped real mounts, gradient
// usage meters, capacity figures.

#pragma once

#include <maya/maya.hpp>

#include "../../core/metrics.hpp"
#include "../theme.hpp"
#include "../fmt.hpp"
#include "meter.hpp"
#include "spark.hpp"
#include "graph.hpp"
#include "panel.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace rockbottom::ui {

class DiskPanel {
    const std::vector<DiskInfo>& disks_;
    const DiskIO& io_;
    bool two_up_ = true;   // pack mounts two per row (wide terminals)
    int graph_h_ = 0;      // >0: draw an I/O area-graph this tall on top

public:
    DiskPanel(const std::vector<DiskInfo>& d, const DiskIO& io, bool two_up = true,
              int graph_h = 0)
        : disks_(d), io_(io), two_up_(two_up), graph_h_(std::max(0, graph_h)) {}

    operator maya::Element() const { return build(); }

    [[nodiscard]] maya::Element build() const {
        using namespace maya;
        using namespace maya::dsl;

        std::vector<Element> rows;

        // Wide/graph mode: a system I/O mountain (read fill + write overlay)
        // above the live rate row and the mount meters. A peak-labelled y-axis
        // gives the mountain height a real magnitude.
        if (graph_h_ >= 2) {
            float peak = 1.0f;
            for (int i = 0; i < io_.hist_len; ++i)
                peak = std::max({peak,
                                 io_.read_history[static_cast<std::size_t>(i)],
                                 io_.write_history[static_cast<std::size_t>(i)]});
            static thread_local std::array<float, 48> rn{}, wn{};
            for (int i = 0; i < io_.hist_len; ++i) {
                rn[static_cast<std::size_t>(i)] = io_.read_history[static_cast<std::size_t>(i)] / peak;
                wn[static_cast<std::size_t>(i)] = io_.write_history[static_cast<std::size_t>(i)] / peak;
            }
            std::string peak_lbl = std::string(humanize_rate(ByteRate{peak}));
            std::vector<Element> axis;
            for (int r = 0; r < graph_h_; ++r) {
                std::string lbl = r == 0 ? peak_lbl : r == graph_h_ - 1 ? "0" : "";
                axis.push_back((text(lbl) | nowrap | fgc(pal::faint)
                                | w_<6> | justify(Justify::End)).build());
            }
            Graph g{rn.data(), io_.hist_len};
            g.fill().rows(graph_h_).color(pal::sky)
             .overlay(wn.data(), io_.hist_len, pal::pink);
            rows.push_back((h(
                v(std::move(axis)) | w_<6>,
                Element{g} | grow(1)
            ) | gap(1) | height(graph_h_)).build());
        }

        // ── Row 1: live whole-system I/O rates ── rendered exactly like a
        // network interface row: peak-normalized rx/tx-style sparks with the
        // baseline mode keeping a faint ▁ track alive when the disk idles,
        // so the graph never vanishes.
        {
            float peak = 1.0f;
            for (int i = 0; i < io_.hist_len; ++i)
                peak = std::max({peak,
                                 io_.read_history[static_cast<std::size_t>(i)],
                                 io_.write_history[static_cast<std::size_t>(i)]});
            std::array<float, 48> rn{}, wn{};
            for (int i = 0; i < io_.hist_len; ++i) {
                rn[static_cast<std::size_t>(i)] = io_.read_history[static_cast<std::size_t>(i)] / peak;
                wn[static_cast<std::size_t>(i)] = io_.write_history[static_cast<std::size_t>(i)] / peak;
            }
            std::string rd = std::string(humanize_rate(io_.read));
            std::string wr = std::string(humanize_rate(io_.write));
            std::array<float, 48> rna = rn, wna = wn;
            int hl = io_.hist_len;
            rows.push_back(Element{ComponentElement{
                .render = [=](int w, int) -> Element {
                    // Mirror net_panel's row: label(8) + ▼(1)+rate(7) + spark
                    // + ▲(1)+rate(7) + spark, sparks splitting the slack.
                    const bool show_spark = w >= 34;
                    int fixed = 8 + 1 + 7 + 2 + 7 + 5;
                    int slack = std::max(0, w - fixed);
                    int each = show_spark ? slack / 2 : 0;
                    std::vector<Element> cols;
                    cols.push_back((text("I/O") | nowrap | Bold | fgc(pal::disk_ac) | w_<8>).build());
                    cols.push_back((text("▼") | nowrap | fgc(pal::sky)).build());
                    cols.push_back((text(rd) | nowrap | fgc(pal::text) | w_<7>).build());
                    if (each > 0)
                        cols.push_back(Spark{rna.data(), hl}.cells(each).color(pal::sky)
                                           .baseline(true).build_fixed());
                    cols.push_back((text("▲") | nowrap | fgc(pal::pink)).build());
                    cols.push_back((text(wr) | nowrap | fgc(pal::text) | w_<7>).build());
                    if (each > 0)
                        cols.push_back(Spark{wna.data(), hl}.cells(each).color(pal::pink)
                                           .baseline(true).build_fixed());
                    return (h(std::move(cols)) | gap(1)).build();
                },
            }});
        }

        // ── Mounts: two per row across the full width ──
        if (disks_.empty()) {
            rows.push_back((text("no mounted disks") | fgc(pal::dim)).build());
        } else {
            auto mount_cell = [&](const DiskInfo& d) -> Element {
                const double f = d.usage().v;
                // Label column matches the I/O row's (8 cells) so both rows
                // hang on the same left rail.
                std::string mnt = d.mount.size() > 8
                    ? "…" + d.mount.substr(d.mount.size() - 7) : d.mount;
                std::string cap = humanize_bytes(d.used) + " / " + humanize_bytes(d.total);
                std::string fs = d.fstype;
                // fstype as a bracketed badge (maya Badge idiom): dim caps,
                // accent label — a quiet tag instead of loose grey text.
                // NOTE: capture by VALUE — this lambda is copied into a
                // ComponentElement render fn that runs after locals die.
                auto fs_badge = [fs]() -> Element {
                    std::string content = "[";
                    std::vector<StyledRun> runs;
                    runs.push_back({0, 1, Style{}.with_fg(pal::faint)});
                    std::size_t off = content.size();
                    content += fs;
                    runs.push_back({off, fs.size(), Style{}.with_fg(mix(pal::disk_ac, pal::dim, 0.4))});
                    off = content.size();
                    content += "]";
                    runs.push_back({off, 1, Style{}.with_fg(pal::faint)});
                    return Element{TextElement{.content = std::move(content), .style = {},
                                               .wrap = TextWrap::NoWrap, .runs = std::move(runs)}};
                };
                if (two_up_) {
                    return (h(
                        text(mnt) | nowrap | fgc(pal::disk_ac) | w_<11>,
                        text(fmt::pct_pad(f)) | nowrap | fgc(load_color(f)) | w_<5>,
                        Meter{f}.width(10),
                        text(cap) | nowrap | fgc(pal::text) | w_<12>,
                        fs_badge()
                    ) | gap(1)).build();
                }
                // One mount per row, on the SAME grid as the I/O row above:
                //   I/O   ▼ 8.8M/s ▂▃▁  ▲ 320K/s ▁▁▁
                //   /       89%    ███████  411G / 460G  [apfs]
                // label(8) · numeric right-aligned to the rate column's right
                // edge (▼+gap+rate = 9 cells) · meter starts where the first
                // spark starts · capacity + fs badge at the right end.
                return Element{ComponentElement{
                    .render = [=](int w, int) -> Element {
                        const bool show_fs  = w >= 44;
                        const bool show_cap = w >= 32;
                        const bool show_pct = w >= 20;
                        // Fixed fstype cell width so EVERY row's meter has the
                        // same width and the capacity/badge columns hang on one
                        // rail — a per-row fsw (btrfs=7 vs vfat=6) made each
                        // meter a different length and the whole block ragged.
                        const int fs_cell = 8;   // "[btrfs]" etc, padded
                        int used = 8 + (show_pct ? 9 + 1 : 0)
                                 + (show_cap ? 12 + 1 : 0) + (show_fs ? fs_cell + 1 : 0) + 1;
                        int mw = std::max(4, w - used);
                        std::vector<Element> cols;
                        cols.push_back((text(mnt) | nowrap | fgc(pal::disk_ac) | w_<8>).build());
                        if (show_pct)
                            cols.push_back((text(fmt::pct_pad(f)) | nowrap | fgc(load_color(f))
                                            | width(9) | justify(Justify::End)).build());
                        cols.push_back(Meter{f}.width(mw).build_fixed());
                        if (show_cap)
                            cols.push_back((text(cap) | nowrap | fgc(pal::text)
                                            | w_<12> | justify(Justify::End)).build());
                        if (show_fs)
                            cols.push_back(fs_badge() | width(fs_cell) | justify(Justify::End));
                        return (h(std::move(cols)) | gap(1)).build();
                    },
                }};
            };
            const int n = static_cast<int>(disks_.size());
            const int step = two_up_ ? 2 : 1;
            for (int i = 0; i < n; i += step) {
                std::vector<Element> line;
                line.push_back(mount_cell(disks_[static_cast<std::size_t>(i)]));
                if (two_up_ && i + 1 < n)
                    line.push_back(mount_cell(disks_[static_cast<std::size_t>(i + 1)]));
                rows.push_back((h(line) | gap(3)).build());
            }
        }

        return Panel("◇", "DISK", pal::disk_ac)(std::move(rows));
    }
};

}  // namespace rockbottom::ui
