<h1 align="center">bottom</h1>

<p align="center">
  A system monitor for people who open <code>htop</code>, whisper <b>"what in god's name,"</b> and close it forever.<br>
  It reads the scary numbers <i>for</i> you and says, slowly and kindly: <b>"your computer is fine, buddy."</b>
</p>

<p align="center">
  <i>Built in modern, type-theoretic C++26 on the <a href="https://github.com/1ay1/maya">maya</a> TUI framework — the code did a PhD so you could keep eating crayons.</i>
</p>

---

## Are you the target audience? A quiz

1. When your fan spins up, do you (a) check the process tree, or (b) sniff the
   air and go "is something burning?"
2. Is "have you tried turning it off and on again" not a joke to you but your
   entire IT strategy?
3. Have you ever killed the wrong process with such confidence, and then such
   regret?
4. Do you think "the cloud" is a place with weather?

If you answered anything other than a stunned silence: **welcome home.** You are
exactly who we built this for. We are not mad. We are proud of you for getting
this far into a README.

## The problem

`htop` and `btop` are beautiful. They are also forty gauges, six graphs, and a
300-row table of hexadecimal-looking gibberish, and they answer the question
"**is my computer okay?**" with a confident, resounding *"you figure it out,
champ."*

So you stare. You nod. You develop a deep, respectful fear of the numbers. You
pick a random process, kill it, and either the problem goes away (you are a
wizard) or your audio dies forever (you are not a wizard). This is not a system
monitor. This is a hostage situation with graphs.

## The fix: we read it for you

bottom hires a guy whose entire job is to look at all that garbage and turn to
you and say one (1) sentence:

```
╭──────────────────────────────────────────────────────────────╮
│ ▲ Working hard — CPU is heavily loaded   chrome (pid 4160) …  │
╰──────────────────────────────────────────────────────────────╯
```

That's the **verdict**. It is colour-coded so that even if you cannot read —
and hey, no judgment, you got this far by looking at the shapes — the *colour*
tells you everything:

- 🟢 **Calm** — everything's fine. Put the fire extinguisher down.
- 🟡 **Busy** — something's working. It's allowed. Breathe.
- 🟠 **Stressed** — okay *now* you may look at the thing we highlighted in red.
- 🔴 **Critical** — this is a "close the tabs" situation. You have too many tabs.
  You always have too many tabs.

And the process actually causing the mess? We slap a big `»` on it and paint it
red so you don't have to go down the list pointing at each row whispering "is it
you?? is it YOU???" like a detective who peaked in kindergarten. **It's Chrome.**
It is *always* Chrome. It was Chrome the whole time.

**You read the one sentence. You know. That's it. That's the app.** Everything
below exists for the two days a year you feel brave.

## Press a number, get the whole story (the detail panes)

Every panel has a **full-screen drill-down** that carries *more detail than htop,
btop, or anything else on the market* — and then reads it for you. Hit a number —
or just *click the panel* — and bottom throws the doors open. Every pane has a
system strip up top (host / kernel / uptime / process census) and ends in a
plain-language verdict, so you never leave staring at digits wondering *so what?*

- **`1` CPU** — a big load-over-time mountain with a real y-axis, live total +
  iowait bars, the 1/5/15 load averages **interpreted against your core count**
  (“oversubscribed — tasks are waiting for a free core”), package temp, a
  **distribution strip** (busiest / quietest / median / average / spread / active
  cores — it'll even call out a single-threaded hog pinning one core), and **every
  core** as its own labelled meter with its live clock speed.
- **`2` MEMORY** — the usage trend graph (spot a leak before it OOMs you), a real
  physical breakdown (used / cache / buffers / available) with the *available*
  verdict that actually matters, the full swap story with a thrash detector, the
  kernel's PSI memory-pressure numbers, **and the top memory-hungry processes
  right in the pane** so you don't go hunting.
- **`3` NETWORK** — an all-interfaces aggregate line (total down/up, lifetime
  bytes, links up), then **every interface** with live rx/tx sparklines, lifetime
  totals, a burst detector, and up/down state.
- **`4` GPU** — **it replaces nvtop entirely.** Utilisation-over-time graph, core
  + VRAM meters, a full telemetry strip (temperature, power draw vs limit, core
  & memory clocks, fan, perf-state, NVENC/NVDEC encoder/decoder load), the
  processes actually holding VRAM, and a verdict (“▲ VRAM is nearly full” /
  “pinned at full load — this is your bottleneck”). Works with NVIDIA (via
  `nvidia-smi`), AMD (`amdgpu` sysfs) and Intel; whatever your card can't report
  is simply left out. Multiple GPUs stack.
- **`5` DISK** — system-wide read/write sparklines, I/O pressure with a
  bottleneck verdict, **every filesystem** with its backing device, free space,
  used/size, fstype and a fullness meter, the fullest-mount callout, and the
  **processes actually driving disk traffic** right now.
- **`6` / `Enter` PROCESS** — drill into the selected process: full command line,
  cpu + memory bars, **its rank against every other process** (so you instantly
  know if it's *the* hog: “#1 CPU consumer on the machine right now”), disk
  read/write, thread count, a *plain-language* run-state (“◆ uninterruptible —
  wedged on I/O, can't even be killed”, not a cryptic `D`), owner, and its
  listening ports. `x`/`K` still end it from right here; `↑↓` walks the list.

Every pane is **responsive** (it reflows for your terminal — more core columns on
a wide screen, tighter layout on a small one) and **scrollable** (`↑↓` / `PgUp` /
`PgDn` / `g` / `G` / the wheel, with a live scrollbar), so no matter how dense the
data or how small the window, nothing ever clips off the edge. `Esc` (or a click)
closes; the number keys switch panes without going back. It's the “okay, tell me
*everything*” button — and it's gorgeous.

## Everything else (for your brave days)

- **Verdict banner** — Calm / Busy / Stressed / Critical, with the guilty party
  named out loud in front of everyone.
- **PSI pressure chips** — the Linux kernel keeps a little diary of how much your
  stuff is stuck waiting around (`/proc/pressure`). "tasks stalled on I/O 40% of
  the last 10s" is the kernel tattling on your disk. htop and btop are too polite
  to ask. We are not polite. We read the diary.
- **CPU** — one big number ("am I busy?"), a bunch of little numbers ("which of
  my brains is busy?"), and a graph shaped like a mountain range that goes up
  when you do things and down when you don't. It's basically a mood ring.
- **Memory** — RAM + swap meters, plus the cache/available breakdown, so you can
  finally learn the sacred truth: **"used" memory is a filthy lie.** Linux hoards
  RAM like a dragon on a pile of gold. It is *supposed* to be almost full. Stop
  buying more RAM. Put the credit card down.
- **Disk** — read/write speeds and your actual drives, fullest one first
  (btrfs subvolumes deduped, so you don't see the same disk nine times and think
  you own nine computers). Answers "why slow" and "am I out of room" in one look.
- **Network** — download ▼ and upload ▲ per interface, with cute little graphs.
  Watch the number go up. Feel like a hacker in a movie. You've earned it.
- **Processes** — pick things, filter things, and **kill** things. The loudest
  one comes pre-highlighted so you kill the *right* thing for once in your life.
  Dots tell you who's actually working (●) vs. who's just stuck waiting on the
  disk (◆), so you stop murdering sleeping programs on a hunch.
- **Per-process disk I/O** — the real, honest "who is sanding my SSD down to a
  nub" number, straight from `/proc/<pid>/io`. Sort by it with `i`. The verdict
  will even hand you the culprit's name when the disk is the problem. It's a
  backup you set up in 2019 and forgot. It's fine. It loves you. It just wanted
  attention.
- **Ports** — which program is sitting on `:8080` like a smug little goblin
  (`:8080 +2`), so the next time you get "address already in use" you can find
  the goblin instead of restarting your entire machine like a caveman.
  Sortable with `o`.
- **Battery** — a charge chip in the corner, because sometimes "why is it slow"
  has the deeply humbling answer "it's on 3% and screaming for a charger."

## "Wait, is it actually smart underneath, or just mean?" Both.

You get to be casual because the code is a paranoid genius. Every number has a
**type that knows what it is**, so the program *physically cannot* mix up bytes
and hertz and tell you your hard drive runs at 3.6 gigabytes-per-second of clock
speed. It won't compile. The compiler stands at the door like a bouncer and goes
"nah."

```cpp
using Bytes = Strong<BytesTag, std::uint64_t>;   // storage / memory
using Hertz = Strong<HertzTag, std::uint64_t>;    // clock speed
struct Ratio { double v; };                        // a proportion, clamped [0,1]
template <class Q> struct Rate { double per_sec; }; // Q, but per second

// The ONE legal way to turn Bytes into Bytes-per-second has a name and a
// hall pass, signed by a teacher:
constexpr ByteRate rate(Bytes delta, double dt_sec) noexcept;
```

That's why `4.2G` and `118M/s` and `3.60GHz` are always right — not because a
tired human remembered the unit at 2am, but because the *type* remembered it,
forever, so nobody has to. You are safe here. The nerds have handled it.

## Architecture (you may skip this; you were going to anyway)

bottom is a maya **Elm-style `Program`** — pure functions, no sneaky mutations,
the kind of clean code that would make your therapist proud:

| Layer | File | Role |
|-------|------|------|
| Units | `src/units.hpp` | Strong dimensional types + the humanizers that turn scary numbers into `4.2G` |
| Data | `src/metrics.hpp` | Pure `Snapshot` value types + the almighty `Verdict` |
| Sampler | `src/sampler.{hpp,cpp}` | The one grubby place that touches the real world (`/proc`, `/sys`) |
| App | `src/app.cpp` | `Model` + `Msg` + `update` + a pure `view` |

`update()` ticks once a second like a very calm heartbeat. `view()` is a pure
function of the state. The sampler remembers the last sample so every rate is a
real delta and not a wild guess. It's genuinely tidy in here. Please take your
shoes off.

## Building

Needs a C++26 compiler (GCC 15+; built on GCC 16) and CMake 3.28+. Yes it's a
fancy compiler. The type theory has *demands.* It's an artist.

```bash
git clone --recurse-submodules <this repo>
cd bottom
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/bottom
```

Cloned it and forgot `--recurse-submodules`, you beautiful disaster?

```bash
git submodule update --init --recursive
```

There. We fixed it. You're welcome. We're not mad.

## Keys (mostly one hand, we know your other hand is holding a snack)

| Key | Action |
|-----|--------|
| `↑↓` / `j k` | pick a process (`j k` is for people who use vim and won't shut up about it) |
| `/` | filter by name or pid (`Esc` un-does it when you inevitably typo) |
| `x` / `Del` | politely ask a process to leave (SIGTERM — it asks you first) |
| `K` | *aggressively* remove a process (SIGKILL — still asks first, we're not savages) |
| `y` / `n` | yes commit the crime / no I panicked nevermind |
| `s` | cycle sort · `c` cpu · `m` mem · `i` i/o · `n` name · `P` pid · `o` port |
| `1`–`6` / `Enter` | open a detail pane (cpu · mem · net · gpu · disk · process) |
| `↑↓` / `PgUp`/`PgDn` / `g`/`G` | scroll the detail pane (wheel works too) |
| `p` / `Space` | pause / resume (freeze the chaos so you can point at it) |
| `?` / `h` | help, for when all of this immediately leaves your brain |
| `q` / `Esc` | leave, go outside, touch grass |

**Or just use your mouse, you absolute animal.** Full mouse support, zero misses:

| Do this | Get that |
|---------|----------|
| **click a process** | selects it (no more arrow-key marathons) |
| **click a column header** | sorts by it (CPU, MEM, DISK, PORT, NAME) |
| **click a panel** | opens its detail pane |
| **click a footer hint** | fires that action — `?·help`, `space·pause`, `s·sort`, `x·end`, `K·kill`, `/·filter`, `q·quit` |
| **right-click a process** | arms an end (SIGTERM — still asks first) |
| **scroll wheel** | rolls the process list |

The click-to-row math is anchored to the layout so a click lands on *exactly* the
row you clicked, at every terminal size. No "close enough." We checked.

## Platform

Linux only. It reads `/proc` and `/sys` with its bare hands — no ncurses, no
dependencies, no phoning home to sell your fan speeds to advertisers. Runs on a
normal `x86_64` kernel. On Windows or macOS? This isn't for you, but honestly,
props for reading a Linux README top to bottom. That's the spirit.

## License

MIT. Do whatever you want, we're not your dad. Vendored maya is MIT too.
Everybody's chill. Go be free.

---

<p align="center">
  <i>bottom: for when "is my computer okay?" deserved an actual answer, and you deserved to not have to think about it.</i>
</p>
