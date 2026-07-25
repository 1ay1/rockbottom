// tests/core_grid_test.cpp — headless render checks for the per-core views.
//
// These are the regressions that issues #2 and #3 were about, pinned as
// assertions against the REAL widgets rendered onto a real canvas:
//
//   #2  every per-core row must put its numeric columns at the same x. The
//       old grid dropped the temp cell to width(0) on cores with no probe and
//       let flex hand the rounding residue to the leading column, so rows
//       ragged by up to 5 cells. We render a machine with SPARSE per-core
//       temps — the exact shape of the reporter's screenshot — and assert the
//       '%' glyphs form a straight column.
//
//   #3  a heterogeneous machine must label every core with its cluster, and a
//       homogeneous one must not gain a phantom column.
//
// Run: cmake --build build --target rb_tests && ./build/rb_tests

#include <maya/maya.hpp>

#include "../src/core/metrics.hpp"
#include "../src/core/platform/linux/topology.hpp"
#include "../src/ui/widgets/cpu_panel.hpp"
#include "../src/ui/widgets/detail/cpu.hpp"

#include <cctype>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace rockbottom;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %s %s\n", ok ? "\x1b[32mPASS\x1b[0m" : "\x1b[31mFAIL\x1b[0m", what.c_str());
    if (!ok) ++failures;
}

// Render an element at a fixed size and return it as plain text rows.
std::vector<std::string> render_rows(const maya::Element& e, int w, int h) {
    maya::StylePool pool;
    maya::Canvas canvas(w, h, &pool);
    maya::Theme theme;
    maya::render_tree(e, canvas, pool, theme);
    std::vector<std::string> out;
    for (int y = 0; y < canvas.height(); ++y) {
        std::string row;
        for (int x = 0; x < canvas.width(); ++x) {
            const char32_t c = canvas.get(x, y).character;
            // Fold every non-ASCII glyph (meters, sparks, box art) to a filler
            // so column arithmetic stays in cells, not bytes.
            row += (c < 128 && c != 0) ? static_cast<char>(c) : '.';
        }
        while (!row.empty() && row.back() == ' ') row.pop_back();
        out.push_back(row);
    }
    return out;
}

// Column offsets of every '%' in a row.
std::vector<int> pct_columns(const std::string& row) {
    std::vector<int> cols;
    for (std::size_t i = 0; i < row.size(); ++i)
        if (row[i] == '%') cols.push_back(static_cast<int>(i));
    return cols;
}

// Is this row a PER-CORE row?
//
// Both views put the core id first, so after stripping the panel border and
// indentation a core row starts with a digit ("7", "12·E") and carries a load
// percentage. Everything else in the pane — the ALL header, RIGHT NOW,
// DISTRIBUTION, the cluster headings — starts with a letter, so this cleanly
// isolates the grid we care about without hardcoding coordinates.
bool is_core_row(const std::string& row) {
    std::size_t i = 0;
    while (i < row.size() && !std::isalnum(static_cast<unsigned char>(row[i]))) ++i;
    if (i >= row.size() || !std::isdigit(static_cast<unsigned char>(row[i]))) return false;
    return row.find('%') != std::string::npos;
}

// A synthetic machine: `n` cores, optionally hybrid, with temps on only the
// cores whose index satisfies `temp_every` — the sparse pattern that exposed
// the alignment bug (the screenshot had temps on ~1 core in 4).
CpuInfo make_cpu(int n, bool hybrid, int temp_every) {
    CpuInfo c;
    c.model = "Test CPU";
    c.logical = n;
    c.total = Ratio{0.42};
    c.cores.resize(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        CpuCore& k = c.cores[static_cast<std::size_t>(i)];
        k.usage = Ratio{static_cast<double>((i * 37) % 100) / 100.0};
        k.freq = Hertz{2'400'000'000ULL + static_cast<std::uint64_t>(i) * 100'000'000ULL};
        k.hist_len = 20;
        for (int j = 0; j < 20; ++j)
            k.history[static_cast<std::size_t>(j)] = static_cast<float>(((i + j) % 10) / 10.0);
        if (temp_every > 0 && i % temp_every == 0) k.temp_c = 60.0f + static_cast<float>(i % 8);
        if (hybrid) {
            // Half performance, half efficiency — Linux ordering (P first).
            k.kind = i < n / 2 ? CoreKind::Perf : CoreKind::Eff;
        }
    }
    if (hybrid) {
        c.perf_cores = n / 2;
        c.eff_cores = n - n / 2;
        c.perf_label = "Performance";
        c.eff_label = "Efficiency";
    }
    return c;
}

Snapshot make_snapshot(CpuInfo cpu) {
    Snapshot s;
    s.cpu = std::move(cpu);
    s.hostname = "testbox";
    s.kernel = "Test 1.0";
    return s;
}

// ── #2: the numeric columns must line up ────────────────────────────────
// Collect the rows that carry per-core figures and assert every one of them
// places its '%' at the same set of columns. A ragged grid produces different
// offsets per row — which is exactly what the reporter saw.
void test_alignment(const std::string& name, const std::vector<std::string>& rows) {
    std::vector<std::vector<int>> shapes;
    for (const std::string& r : rows) {
        if (!is_core_row(r)) continue;
        const std::vector<int> cols = pct_columns(r);
        if (!cols.empty()) shapes.push_back(cols);
    }
    check(shapes.size() >= 2, name + ": found per-core rows to compare (" +
                                  std::to_string(shapes.size()) + ")");
    if (shapes.size() < 2) return;

    // Rows in a grid can hold a different NUMBER of cells (the last row may be
    // short), so compare each row's leading columns against the widest row's.
    std::size_t widest = 0;
    for (std::size_t i = 1; i < shapes.size(); ++i)
        if (shapes[i].size() > shapes[widest].size()) widest = i;

    bool aligned = true;
    std::string detail;
    for (const std::vector<int>& s : shapes) {
        for (std::size_t k = 0; k < s.size() && k < shapes[widest].size(); ++k) {
            if (s[k] != shapes[widest][k]) {
                aligned = false;
                detail = " (col " + std::to_string(k) + ": " + std::to_string(s[k]) +
                         " vs " + std::to_string(shapes[widest][k]) + ")";
                break;
            }
        }
        if (!aligned) break;
    }
    check(aligned, name + ": every core row's % lands in the same column" + detail);
}

void dump(const std::string& title, const std::vector<std::string>& rows) {
    std::printf("\n\x1b[2m--- %s ---\x1b[0m\n", title.c_str());
    for (const std::string& r : rows)
        if (!r.empty()) std::printf("\x1b[2m|\x1b[0m%s\n", r.c_str());
}

// Render the CPU detail pane body for a snapshot at a given size.
std::vector<std::string> render_detail(const Snapshot& s, int w, int h) {
    ui::detail::Ctx cx = ui::detail::Ctx::make(w, 50, 0);
    std::vector<maya::Element> col = ui::detail::cpu_body(s, cx);
    return render_rows(maya::dsl::v(std::move(col)).build(), w, h);
}

// ── topology fixtures ────────────────────────────────────────
// An in-memory sysfs: path -> contents. Lets the classifier be driven against
// the exact file layout of machines we don't have on hand.
using FakeFs = std::map<std::string, std::string>;

topo::ReadFile fs_reader(const FakeFs& fs) {
    return [&fs](const std::string& p) -> std::string {
        auto it = fs.find(p);
        return it == fs.end() ? std::string() : it->second;
    };
}

// Standard topology/ files: `n` logical cpus over `n/smt` physical cores.
void add_cpus(FakeFs& fs, int n, int smt = 1) {
    for (int i = 0; i < n; ++i) {
        const std::string b = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/";
        fs[b + "core_id"] = std::to_string(i / smt);
        fs[b + "physical_package_id"] = "0";
    }
}

void add_scalar(FakeFs& fs, const std::string& suffix, const std::vector<long>& vals) {
    for (std::size_t i = 0; i < vals.size(); ++i)
        fs["/sys/devices/system/cpu/cpu" + std::to_string(i) + suffix] =
            std::to_string(vals[i]) + "\n";
}

std::string kinds_str(const topo::Topology& t) {
    if (t.kind.empty()) return "(homogeneous)";
    std::string s;
    for (CoreKind k : t.kind)
        s += k == CoreKind::Perf ? 'P' : k == CoreKind::Eff ? 'E' : '?';
    return s;
}

void expect_kinds(const std::string& name, const topo::Topology& t, const std::string& want) {
    const std::string got = kinds_str(t);
    check(got == want, name + ": " + got + (got == want ? "" : " (wanted " + want + ")"));
}

void test_topology() {
    std::printf("\nissue #3 — Linux topology classifier:\n");

    // Intel Core i7-12700H — 6 P cores with SMT (cpu0-11) + 8 E cores
    // (cpu12-19). The kernel names both clusters, so this must be exact.
    {
        FakeFs fs;
        add_cpus(fs, 20);
        fs["/sys/devices/cpu_core/cpus"] = "0-11\n";
        fs["/sys/devices/cpu_atom/cpus"] = "12-19\n";
        const topo::Topology t = topo::classify(20, fs_reader(fs));
        expect_kinds("alder lake i7-12700H", t, "PPPPPPPPPPPPEEEEEEEE");
        check(t.perf_label == "Performance", "alder lake: performance cluster named");
    }

    // A sparse/comma cpulist — the format is a list, not a single range.
    {
        FakeFs fs;
        add_cpus(fs, 8);
        fs["/sys/devices/cpu_core/cpus"] = "0-3,6\n";
        fs["/sys/devices/cpu_atom/cpus"] = "4,5,7\n";
        const topo::Topology t = topo::classify(8, fs_reader(fs));
        expect_kinds("sparse cpulist", t, "PPPPEEPE");
    }

    // ARM big.LITTLE (Snapdragon-style): 4 little + 4 big by DT capacity.
    {
        FakeFs fs;
        add_cpus(fs, 8);
        add_scalar(fs, "/cpu_capacity", {256, 256, 256, 256, 1024, 1024, 1024, 1024});
        const topo::Topology t = topo::classify(8, fs_reader(fs));
        expect_kinds("big.LITTLE capacity", t, "EEEEPPPP");
    }

    // Max-frequency inference, when neither of the better sources exists.
    {
        FakeFs fs;
        add_cpus(fs, 8);
        add_scalar(fs, "/cpufreq/cpuinfo_max_freq",
                   {2000000, 2000000, 2000000, 2000000, 3600000, 3600000, 3600000, 3600000});
        const topo::Topology t = topo::classify(8, fs_reader(fs));
        expect_kinds("max-freq inference", t, "EEEEPPPP");
        check(t.eff_label == "Slow", "max-freq: cluster labels are hedged (" + t.eff_label + ")");
    }

    // Homogeneous EPYC with binning scatter: clocks differ by ~1%, which is
    // NOT a cluster. Calling this hybrid would put a bogus P/E column on every
    // ordinary server — the regression this threshold exists to prevent.
    {
        FakeFs fs;
        add_cpus(fs, 16, 2);
        add_scalar(fs, "/cpufreq/cpuinfo_max_freq",
                   {3700000, 3700000, 3693000, 3700000, 3700000, 3688000, 3700000, 3700000,
                    3700000, 3695000, 3700000, 3700000, 3700000, 3700000, 3700000, 3690000});
        const topo::Topology t = topo::classify(16, fs_reader(fs));
        expect_kinds("homogeneous EPYC (binning scatter)", t, "(homogeneous)");
    }

    // Boost-clock quirk: one core certified 200MHz higher (Intel Turbo Boost
    // Max / AMD preferred core). Still one cluster.
    {
        FakeFs fs;
        add_cpus(fs, 8);
        add_scalar(fs, "/cpufreq/cpuinfo_max_freq",
                   {5000000, 4800000, 4800000, 4800000, 4800000, 4800000, 4800000, 4800000});
        const topo::Topology t = topo::classify(8, fs_reader(fs));
        expect_kinds("favoured-core boost quirk", t, "(homogeneous)");
    }

    // SMT sibling map: "Core 3" from Intel coretemp must reach BOTH threads.
    {
        FakeFs fs;
        add_cpus(fs, 16, 2);
        const topo::Topology t = topo::classify(16, fs_reader(fs));
        auto it = t.siblings.find(3);
        const bool paired = it != t.siblings.end() && it->second.size() == 2 &&
                            it->second[0] == 6 && it->second[1] == 7;
        check(paired, "physical core 3 maps to logical cpus 6 and 7");
    }

    // Multi-socket: both sockets have a "core 0", which must NOT collide.
    {
        FakeFs fs;
        for (int i = 0; i < 8; ++i) {
            const std::string b = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/";
            fs[b + "core_id"] = std::to_string(i % 4);
            fs[b + "physical_package_id"] = std::to_string(i / 4);
        }
        const topo::Topology t = topo::classify(8, fs_reader(fs));
        check(t.siblings.size() == 8, "dual-socket: 8 distinct physical cores (got " +
                                          std::to_string(t.siblings.size()) + ")");
    }

    // Container with a masked /sys: no crash, no phantom cluster.
    {
        FakeFs fs;
        const topo::Topology t = topo::classify(4, fs_reader(fs));
        expect_kinds("masked /sys (container)", t, "(homogeneous)");
        check(t.phys.empty(), "masked /sys: no physical-core map");
    }

    // Partially readable capacity: refusing to guess beats mislabelling the
    // cores whose files didn't answer.
    {
        FakeFs fs;
        add_cpus(fs, 8);
        add_scalar(fs, "/cpu_capacity", {1024, 1024, 256, 256});   // only 4 of 8
        const topo::Topology t = topo::classify(8, fs_reader(fs));
        expect_kinds("partial capacity read", t, "(homogeneous)");
    }
}

}  // namespace

int main() {
    std::printf("\n\x1b[1mper-core grid tests\x1b[0m\n\n");

    test_topology();

    // ── issue #2: sparse temps must not shift rows ──────────────────
    // 130 cols keeps the pane in SINGLE-column mode (ultrawide splits at 146),
    // so the per-core grid owns whole rows and every one of them is compared.
    {
        std::printf("issue #2 — detail pane, 22 cores, temps on 1 in 4:\n");
        const auto rows = render_detail(make_snapshot(make_cpu(22, false, 4)), 130, 60);
        test_alignment("detail/sparse-temps", rows);
        dump("detail pane (22 cores, sparse temps)", rows);
    }

    // ── same shape, no temps at all ──────────────────────────────
    {
        std::printf("\nissue #2 — detail pane, 22 cores, no per-core temps:\n");
        const auto rows = render_detail(make_snapshot(make_cpu(22, false, 0)), 130, 60);
        test_alignment("detail/no-temps", rows);
    }

    // ── the reporter's exact shape: temps on a sparse, IRREGULAR subset ────
    // In the screenshot cores 0-8, 12, 16 and 20 had a °C figure and the rest
    // did not — the row-by-row asymmetry that made the old grid rag.
    {
        std::printf("\nissue #2 — irregular temp coverage (screenshot shape):\n");
        CpuInfo cpu = make_cpu(22, false, 0);
        for (int i : {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20})
            cpu.cores[static_cast<std::size_t>(i)].temp_c = 60.0f + static_cast<float>(i % 9);
        const auto rows = render_detail(make_snapshot(cpu), 130, 60);
        test_alignment("detail/irregular-temps", rows);
        dump("detail pane (irregular temps)", rows);
    }

    // ── narrow / awkward widths must stay aligned too ──────────────────
    // Column count and spark width both change across these; the alignment
    // contract must hold at every one, including widths that do not divide
    // evenly by the column count (where flex used to smear the residue).
    {
        std::printf("\nissue #2 — alignment holds across widths:\n");
        for (int w : {71, 83, 100, 107, 128, 141, 173, 199}) {
            const auto rows = render_detail(make_snapshot(make_cpu(22, false, 4)), w, 70);
            test_alignment("detail/w=" + std::to_string(w), rows);
        }
        for (int w : {64, 90, 113, 130, 161}) {
            const CpuInfo cpu = make_cpu(22, false, 4);
            const auto rows = render_rows(ui::CpuPanel(cpu, 3, 46, 4).build(), w, 24);
            test_alignment("panel/w=" + std::to_string(w), rows);
        }
    }

    // ── dashboard panel grid ───────────────────────────────────
    {
        std::printf("\nissue #2 — dashboard CPU panel, 22 cores, 3 columns:\n");
        const CpuInfo cpu = make_cpu(22, false, 4);
        const auto rows = render_rows(ui::CpuPanel(cpu, 3, 46, 4).build(), 130, 24);
        test_alignment("panel/3-col", rows);
        dump("dashboard panel (22 cores)", rows);
    }

    // ── issue #3: hybrid machines label every core ──────────────────
    {
        std::printf("\nissue #3 — hybrid machine, per-core P/E labels:\n");
        const auto rows = render_detail(make_snapshot(make_cpu(16, true, 0)), 130, 70);

        int p_tags = 0, e_tags = 0;
        for (const std::string& r : rows)
            for (std::size_t i = 1; i + 1 < r.size(); ++i) {
                // ids render as "NN.P" / "NN.E" (the · folds to '.').
                if (r[i] == '.' && (r[i + 1] == 'P' || r[i + 1] == 'E')
                    && std::isdigit(static_cast<unsigned char>(r[i - 1]))) {
                    if (r[i + 1] == 'P') ++p_tags; else ++e_tags;
                }
            }
        check(p_tags == 8, "8 cores tagged P (got " + std::to_string(p_tags) + ")");
        check(e_tags == 8, "8 cores tagged E (got " + std::to_string(e_tags) + ")");

        // The clusters must be grouped under their own headings, and the
        // pane must name both — that IS the feature issue #3 asked for.
        bool perf_heading = false, eff_heading = false;
        for (const std::string& r : rows) {
            if (r.find("Performance") != std::string::npos) perf_heading = true;
            if (r.find("Efficiency") != std::string::npos)  eff_heading = true;
        }
        check(perf_heading, "performance cluster is titled");
        check(eff_heading, "efficiency cluster is titled");
        test_alignment("detail/hybrid", rows);
        dump("detail pane (hybrid 8P+8E)", rows);
    }

    // ── the panel shows the class too, not just the drill-down ──────────
    {
        std::printf("\nissue #3 — dashboard panel tags cores:\n");
        const CpuInfo cpu = make_cpu(16, true, 0);
        const auto rows = render_rows(ui::CpuPanel(cpu, 2, 46, 4).build(), 120, 20);
        int tags = 0;
        for (const std::string& r : rows)
            for (std::size_t i = 1; i + 1 < r.size(); ++i)
                if (r[i] == '.' && (r[i + 1] == 'P' || r[i + 1] == 'E')
                    && std::isdigit(static_cast<unsigned char>(r[i - 1]))) ++tags;
        check(tags == 16, "all 16 cores tagged in the panel (got " + std::to_string(tags) + ")");
        test_alignment("panel/hybrid", rows);
        dump("dashboard panel (hybrid)", rows);
    }

    // ── homogeneous machines must NOT grow cluster chrome ───────────────
    {
        std::printf("\nissue #3 — homogeneous machine stays flat:\n");
        const auto rows = render_detail(make_snapshot(make_cpu(8, false, 0)), 130, 60);
        bool has_cluster = false;
        for (const std::string& r : rows)
            if (r.find("PERFORMANCE") != std::string::npos ||
                r.find("Performance") != std::string::npos ||
                r.find("EFFICIENCY") != std::string::npos) has_cluster = true;
        check(!has_cluster, "no cluster headings on a homogeneous machine");
    }

    std::printf("\n%s\n\n", failures ? ("\x1b[31m" + std::to_string(failures) +
                                        " check(s) failed\x1b[0m").c_str()
                                     : "\x1b[32mall checks passed\x1b[0m");
    return failures ? 1 : 0;
}
