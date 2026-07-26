// tests/render_bench.cpp — headless per-frame cost breakdown for the dashboard.
//
// The pty benchmark (/tmp/ab.py) proves rb's CPU grows super-linearly with
// screen AREA where btop stays flat. That points at the per-frame render path,
// not the sampler (`rb --bench` shows sampling is <1% of a core). But a pty
// benchmark can't tell you WHICH phase — view() composition, layout/paint, or
// the diff — is the cost. This does: it builds one synthetic Snapshot with a
// realistic process count, then loops the exact pipeline maya runs each frame,
// timing each phase separately at a chosen size.
//
// Run: cmake --build build --target rb_render_bench -j 8
//      ./build/rb_render_bench 200 80        # cols rows [iters]

#include <maya/maya.hpp>
#include <maya/render/pipeline.hpp>

#include "../src/core/metrics.hpp"
#include "../src/ui/app.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace rockbottom;
using clk = std::chrono::steady_clock;

namespace {

// A Snapshot shaped like a real desktop: ~400 processes in a shallow tree,
// 8 cores with history, a few disks/nets. Enough rows that the proc table
// dominates the frame, which is what happens on a real machine.
Snapshot make_big_snapshot(int nprocs) {
    Snapshot s;
    s.hostname = "bench-host";
    s.kernel = "24.0.0";
    s.uptime_sec = 123456;
    s.proc_count = nprocs;
    s.thread_count = nprocs * 4;
    s.running = 3;

    s.cpu.model = "Apple M-bench 8-core";
    s.cpu.logical = 8;
    s.cpu.perf_cores = 4;
    s.cpu.eff_cores = 4;
    s.cpu.total = Ratio{0.42};
    s.cpu.cores.resize(8);
    for (int i = 0; i < 8; ++i) {
        auto& c = s.cpu.cores[static_cast<std::size_t>(i)];
        c.usage = Ratio{0.1 + 0.08 * i};
        c.freq = Hertz{static_cast<std::uint64_t>(2400 + 40 * i) * 1000000ull};
        c.kind = (i < 4) ? CoreKind::Eff : CoreKind::Perf;
        c.temp_c = 45.0f + i;
        c.phys = i;
        c.hist_len = 48;
        for (int k = 0; k < 48; ++k)
            c.history[static_cast<std::size_t>(k)] = 0.2f + 0.01f * ((i + k) % 30);
    }
    s.cpu.total_hist_len = 48;
    for (int k = 0; k < 96; ++k)
        s.cpu.total_history[static_cast<std::size_t>(k)] = 0.3f + 0.005f * (k % 40);

    s.mem.total = Bytes{16ull << 30};
    s.mem.used = Bytes{9ull << 30};
    s.mem.available = Bytes{7ull << 30};
    s.mem.hist_len = 48;
    for (int k = 0; k < 120; ++k)
        s.mem.usage_history[static_cast<std::size_t>(k)] = 0.5f + 0.004f * (k % 20);

    DiskInfo d;
    d.mount = "/";
    d.total = Bytes{500ull << 30};
    d.used = Bytes{220ull << 30};
    s.disks.push_back(d);

    NetIface n;
    n.name = "en0";
    n.hist_len = 48;
    s.nets.push_back(n);

    s.procs.reserve(static_cast<std::size_t>(nprocs));
    for (int i = 0; i < nprocs; ++i) {
        ProcInfo p;
        p.pid = 100 + i;
        p.ppid = (i < 3) ? 1 : (100 + (i % 8));   // shallow tree
        p.name = "proc" + std::to_string(i);
        p.user = (i % 3 == 0) ? "root" : "ayush";
        p.cmd = "/usr/bin/proc" + std::to_string(i) + " --flag=" + std::to_string(i);
        p.cpu = 0.01 * (i % 97);
        p.rss = Bytes{static_cast<std::uint64_t>((i % 200 + 1)) << 20};
        p.footprint = p.rss;
        p.state = (i % 20 == 0) ? 'R' : 'S';
        p.threads = 1 + (i % 12);
        p.nice = (i % 5) - 2;
        p.cpu_ms = static_cast<std::uint64_t>(i) * 137;
        p.hist_len = 48;
        for (int k = 0; k < 48; ++k)
            p.cpu_history[static_cast<std::size_t>(k)] = 0.01f * ((i + k) % 90);
        s.procs.push_back(std::move(p));
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    int cols = argc > 1 ? std::atoi(argv[1]) : 200;
    int rows = argc > 2 ? std::atoi(argv[2]) : 80;
    int iters = argc > 3 ? std::atoi(argv[3]) : 400;
    int nprocs = argc > 4 ? std::atoi(argv[4]) : 400;

    App::Model m;
    m.snap = make_big_snapshot(nprocs);
    m.width = cols;
    m.height = rows;
    m.refresh_ms = 1000;

    maya::StylePool pool;
    maya::Theme theme;
    // Two canvases so the diff has a realistic previous frame to compare to.
    maya::Canvas front(cols, rows, &pool);
    maya::Canvas back(cols, rows, &pool);
    std::string out;
    out.reserve(1 << 16);
    std::vector<maya::layout::LayoutNode> nodes;

    // Warm up (first frame allocates layout arena, style pool, etc.).
    for (int i = 0; i < 5; ++i) {
        maya::Element e = App::view(m);
        maya::render_tree(e, back, pool, theme, nodes);
    }

    double t_view = 0, t_total = 0;
    std::size_t bytes_sink = 0;
    std::uint64_t build0 = maya::render_detail::rt_build_ns();
    std::uint64_t layout0 = maya::render_detail::rt_layout_ns();
    std::uint64_t paint0 = maya::render_detail::rt_paint_ns();
    for (int i = 0; i < iters; ++i) {
        // Perturb MANY data every frame so the diff is fat (mimics a real
        // sample re-sorting the proc list — nearly every row changes). Env
        // BENCH_CHURN sets how many procs move; default 1 (light).
        static const int churn = [] {
            const char* e = std::getenv("BENCH_CHURN");
            return e ? std::atoi(e) : 1;
        }();
        for (int j = 0; j < churn && j < nprocs; ++j)
            m.snap.procs[static_cast<std::size_t>((i + j) % nprocs)].cpu += 0.001 + 0.0001 * j;
        m.snap_gen++;

        auto a = clk::now();
        maya::Element e = App::view(m);
        auto b = clk::now();
        // Full frame: clear back, paint tree, diff against front → ANSI bytes.
        out.clear();
        maya::RenderPipeline<maya::stage::Idle>::start(back, pool, theme, out)
            .clear()
            .paint(e, nodes)
            .open_frame(false)
            .write_diff(front)
            .close_frame(false);
        auto c = clk::now();

        bytes_sink += out.size();
        t_view += std::chrono::duration<double, std::milli>(b - a).count();
        t_total += std::chrono::duration<double, std::milli>(c - a).count();
        std::swap(front, back);
    }

    std::printf("size %dx%d  procs=%d  iters=%d  (bytes/frame=%zu)\n",
                cols, rows, nprocs, iters, bytes_sink / static_cast<std::size_t>(iters));
    std::printf("  view()       %7.3f ms/frame\n", t_view / iters);
    const double bns = double(maya::render_detail::rt_build_ns() - build0) / 1e6 / iters;
    const double lns = double(maya::render_detail::rt_layout_ns() - layout0) / 1e6 / iters;
    const double pns = double(maya::render_detail::rt_paint_ns() - paint0) / 1e6 / iters;
    std::printf("    build_layout %7.3f ms/frame\n", bns);
    std::printf("    layout       %7.3f ms/frame\n", lns);
    std::printf("    paint        %7.3f ms/frame\n", pns);
    std::printf("  diff+serialize%6.3f ms/frame\n",
                (t_total / iters) - (t_view / iters) - bns - lns - pns);
    std::printf("  TOTAL        %7.3f ms/frame\n", t_total / iters);
    return 0;
}
