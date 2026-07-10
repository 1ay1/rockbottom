<h1 align="center">bottom</h1>

<p align="center">
  A system monitor that <b>tells you what's going on</b> — instead of making you decode it.<br>
  Built in modern, type-theoretic C++26 on the <a href="https://github.com/1ay1/maya">maya</a> TUI framework.
</p>

---

## Why bottom exists

`htop` and `btop` are gorgeous number-soup. You stare at forty gauges and a
200-row process table and *still* have to work out the answer yourself:
**is my machine fine, and if not, what's to blame?**

bottom answers that question **before** it shows you a single gauge:

```
╭──────────────────────────────────────────────────────────────╮
│ ▲ Working hard — CPU is heavily loaded   chrome (pid 4160) …  │
╰──────────────────────────────────────────────────────────────╯
```

The banner at the top is a **verdict** — one plain-language sentence, colour-coded
from calm-green to critical-red. The process actually *driving* that verdict is
auto-highlighted in the table with a `»`. You read one line and you know.
The panels underneath are there when you want the detail — not before.

## What it shows

- **Verdict banner** — Calm / Busy / Stressed / Critical, with the culprit named.
- **PSI pressure chips** — the kernel's own stall accounting (`/proc/pressure`):
  "tasks stalled on I/O 40% of the last 10s" beats guessing from load averages.
  Neither htop nor btop surfaces this.
- **CPU** — aggregate meter, per-core meters, value-colored history sparklines,
  package temperature, load trend arrow.
- **Memory** — RAM + swap meters, cache / buffers / available breakdown.
- **Disk** — live system-wide read/write I/O rates + real mounted filesystems
  (btrfs subvolumes deduped), busiest first.
- **Network** — per-interface ▼rx / ▲tx rates with peak-normalised sparklines.
- **Processes** — interactive: select, filter, sort, and kill — with the
  loudest process flagged » and running/disk-sleep state dots.
- **Ports** — every process's bound TCP/UDP ports (`:8080 +2`), joined from
  `/proc/net/*` socket inodes to `/proc/<pid>/fd` — the `ss -p` trick, live
  in the table, sortable with `o`.
- **Battery** — charge chip in the header when hardware exists.

## Design: type-theoretic core

The metrics layer makes dimensional bugs *unrepresentable*. Every quantity is a
phantom-tagged strong type — you cannot add bytes to hertz, or store a rate as a
total, because those operations don't type-check:

```cpp
using Bytes = Strong<BytesTag, std::uint64_t>;   // storage / memory
using Hertz = Strong<HertzTag, std::uint64_t>;    // clock speed
struct Ratio { double v; };                        // clamped [0,1] proportion
template <class Q> struct Rate { double per_sec; }; // Q per second

// The ONLY sanctioned Bytes → Bytes/s crossing is a named function:
constexpr ByteRate rate(Bytes delta, double dt_sec) noexcept;
```

Human formatting (`4.2G`, `118M/s`, `3.60GHz`) is then correct **by
construction** because it dispatches on the type, not on a hand-remembered unit.

## Architecture

bottom is a maya **Elm-style `Program`** — pure, testable, no hidden mutation:

| Layer | File | Role |
|-------|------|------|
| Units | `src/units.hpp` | Strong dimensional types + humanizers |
| Data | `src/metrics.hpp` | Pure `Snapshot` value types + `Verdict` |
| Sampler | `src/sampler.{hpp,cpp}` | The one impure boundary — reads `/proc` & `/sys`, computes deltas |
| App | `src/app.cpp` | `Model` + `Msg` + `update` + pure `view` |

`update()` is a pure state machine driven by a 1-second `Tick`; `view()` is a
pure function of the model; the sampler owns the previous sample so CPU-busy
fractions, network rates and per-process CPU are honest deltas across ticks.

## Building

Requires a C++26 compiler (GCC 15+; developed on GCC 16) and CMake 3.28+.

```bash
git clone --recurse-submodules <this repo>
cd bottom
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/bottom
```

Already cloned without submodules?

```bash
git submodule update --init --recursive
```

## Keys

| Key | Action |
|-----|--------|
| `↑↓` / `j k` | select a process |
| `/` | filter processes by name or pid (`Esc` clears) |
| `x` / `Del` | end selected process (SIGTERM, with confirm) |
| `K` | force-kill selected (SIGKILL, with confirm) |
| `y` / `n` | confirm / cancel a pending kill |
| `s` | cycle sort · `c` cpu · `m` mem · `n` name · `P` pid · `o` port |
| `p` / `Space` | pause / resume sampling |
| `?` / `h` | help overlay |
| `q` / `Esc` | quit |

## Platform

Linux only — it reads `/proc` and `/sys` directly (no external dependencies,
no ncurses). Tested on a mainline `x86_64` kernel.

## License

MIT. Vendored maya is MIT-licensed by its authors.
