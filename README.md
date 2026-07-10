<h1 align="center">bottom</h1>

<p align="center">
  A system monitor for people who look at <code>htop</code>, go <b>"huh"</b>, and quietly close the terminal.<br>
  It reads the numbers <i>for</i> you and just tells you if your computer is okay.
</p>

<p align="center">
  <i>Built in modern, type-theoretic C++26 on the <a href="https://github.com/1ay1/maya">maya</a> TUI framework — because the code is smart so you don't have to be.</i>
</p>

---

## Who this is for

You know that feeling when your laptop fan spins up like it's trying to
achieve liftoff, and you open a system monitor, and it shows you **forty
gauges, six graphs, and a 300-row table**, and you nod slowly and pretend you
understand, and then you just... restart the machine and hope?

Yeah. This is for you. We love you. You're not dumb, you're **busy**. (You're a
little bit dumb.)

`htop` and `btop` are gorgeous. They are also number-soup served in a firehose.
They show you *everything* and answer *nothing*. You still have to be the
detective. bottom fired the detective and hired a guy who just walks in and says
the answer out loud.

## The one feature that matters

Right at the top, before any gauge, there is a sentence. In English. That a
human wrote-ish:

```
╭──────────────────────────────────────────────────────────────╮
│ ▲ Working hard — CPU is heavily loaded   chrome (pid 4160) …  │
╰──────────────────────────────────────────────────────────────╯
```

That's the **verdict**. It is colour-coded so even if you don't read it, the
*vibe* gets through:

- 🟢 **Calm** — go back to sleep, nothing is on fire.
- 🟡 **Busy** — something's working. Probably fine. Probably.
- 🟠 **Stressed** — okay, *now* look at the thing we highlighted.
- 🔴 **Critical** — this is the part where you do something.

And the process actually *causing* the drama? We put a little `»` next to it and
paint it red so you don't have to squint down 200 rows going "is it you? is it
YOU?" It's Chrome. It's always Chrome.

**You read one line. You know. That's the whole product.** Everything below is
for the three days a year you actually feel like a hacker.

## Everything else it shows (for those three days)

- **Verdict banner** — Calm / Busy / Stressed / Critical, culprit named and
  shamed publicly.
- **PSI pressure chips** — the kernel literally keeps a diary of how much your
  tasks are stuck waiting (`/proc/pressure`). "tasks stalled on I/O 40% of the
  last 10s" is the kernel snitching on your disk. htop and btop don't even ask
  it. We ask it. We're nosy.
- **CPU** — one big meter (the "am I busy" number), per-core meters (the "which
  brain is busy" numbers), a history graph shaped like a little mountain range,
  package temperature, and a load arrow that points up when things get spicy.
- **Memory** — RAM + swap meters, plus the cache/buffers/available breakdown so
  you can learn the ancient truth that **"used" RAM is a lie** and stop panicking
  about it. Linux hoards RAM like a dragon. It's fine.
- **Disk** — live read/write speeds and your real mounted filesystems (btrfs
  subvolumes deduped, because you don't need to see the same disk nine times),
  fullest one first, so "why is it slow" and "am I out of space" are one glance.
- **Network** — per-interface ▼download / ▲upload with little sparkline graphs.
  Watch the number spike and feel powerful.
- **Processes** — select, filter, sort, and **kill** things. The loudest one is
  pre-flagged `»`. Dots tell you who's actually Running (●) versus stuck waiting
  on the disk (◆), so you stop killing innocent sleeping processes out of spite.
- **Per-process disk I/O** — the actual "who is grinding my SSD into powder"
  number, straight from `/proc/<pid>/io` (real block-device traffic, not fibbing
  cache hits). Sort by it with `i`. The verdict even names the thrasher for you
  when the machine is I/O-bound. Spoiler: it's a backup you forgot about.
- **Ports** — which program is squatting on `:8080` (`:8080 +2`), pieced together
  from the guts of `/proc` like the `ss -p` party trick, live in the table,
  sortable with `o`. Great for the eternal "address already in use" rage.
- **Battery** — a little charge chip in the header when you have a battery,
  because sometimes the answer to "why is it slow" is "it's on 4% power-saver,
  genius."

## "But is it *smart* underneath?" Yes. Painfully.

You get to be casual because the code is deeply, insufferably rigorous. Every
number has a **type that knows what it is**, so the program physically cannot mix
up bytes and hertz and hand you "3.6 gigabytes per second of clock speed." It
won't compile. The compiler is the bouncer.

```cpp
using Bytes = Strong<BytesTag, std::uint64_t>;   // storage / memory
using Hertz = Strong<HertzTag, std::uint64_t>;    // clock speed
struct Ratio { double v; };                        // a proportion, clamped [0,1]
template <class Q> struct Rate { double per_sec; }; // Q, but per second

// The ONE legal way to turn Bytes into Bytes-per-second has a name and a
// permission slip:
constexpr ByteRate rate(Bytes delta, double dt_sec) noexcept;
```

So when it prints `4.2G` or `118M/s` or `3.60GHz`, it's right **by
construction** — not because someone remembered the unit at 2am. Someone did
remember it. Once. Forever. In the type.

## Architecture (skip this unless it's one of the three days)

bottom is a maya **Elm-style `Program`** — pure functions, no sneaky mutation,
the good clean kind of code:

| Layer | File | Role |
|-------|------|------|
| Units | `src/units.hpp` | Strong dimensional types + the humanizers that make `4.2G` |
| Data | `src/metrics.hpp` | Pure `Snapshot` value types + the `Verdict` |
| Sampler | `src/sampler.{hpp,cpp}` | The one place that touches the messy real world (`/proc`, `/sys`) |
| App | `src/app.cpp` | `Model` + `Msg` + `update` + a pure `view` |

`update()` is a pure state machine ticking once a second. `view()` is a pure
function of the model. The sampler holds the previous sample so every rate is an
honest delta and not a vibe. It's very tidy in here. Wipe your feet.

## Building

Needs a C++26 compiler (GCC 15+; developed on GCC 16) and CMake 3.28+. Yes,
that's a fancy compiler. The type theory demanded sacrifices.

```bash
git clone --recurse-submodules <this repo>
cd bottom
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/bottom
```

Cloned it and forgot the submodules, like a dumb person (affectionate)?

```bash
git submodule update --init --recursive
```

## Keys (it's mostly one hand)

| Key | Action |
|-----|--------|
| `↑↓` / `j k` | pick a process (vim keys, for the show-offs) |
| `/` | filter by name or pid (`Esc` to un-filter) |
| `x` / `Del` | politely end a process (SIGTERM, asks first) |
| `K` | *rudely* end a process (SIGKILL, still asks first, we're not monsters) |
| `y` / `n` | yes do the murder / no nevermind |
| `s` | cycle sort · `c` cpu · `m` mem · `i` i/o · `n` name · `P` pid · `o` port |
| `p` / `Space` | pause / resume (freeze-frame the chaos) |
| `?` / `h` | help, for when you forget all of the above |
| `q` / `Esc` | leave |

## Platform

Linux only. It reads `/proc` and `/sys` with its bare hands — no ncurses, no
external dependencies, no phoning home. Works on a normal `x86_64` kernel. If
you're on Windows or macOS: this is not for you, but we're flattered you scrolled
this far.

## License

MIT. Do whatever. The vendored maya is MIT too. Everyone's chill.

---

<p align="center">
  <i>bottom: because "is my computer okay?" deserved a straight answer.</i>
</p>
