# Real-time Windows plugins in buzz-remote, browser-only — Design

Date: 2026-10-04
Status: Phase 0 done (go). Phase 1 MVP done, verified in headless Chromium through buzz-remote's engine (the app shell could not be built in that environment); §11 lists what changed against this design. Builds on the offline proof of concept in
[`experiments/boxedwine-vst`](../../); see its
[performance.md](../performance.md) for the
measurements this design relies on. Revised for engine v2
([2026-10-04-engine-v2-ports-sidechain-design.md](https://github.com/dahlgrenmartin/prometheos-apps/blob/main/apps/buzz-remote/docs/superpowers/specs/2026-10-04-engine-v2-ports-sidechain-design.md):
ports, sidechains, latency compensation; §2, §3.3, §3.7).

## 1. Goal

Run an unmodified **32-bit Windows VST2/VST3 plugin as a buzz-remote machine in
real time**, entirely in the browser: static hosting, no backend, no native
helper.

Success means:

- a user installs a Windows plugin (`.dll` or `.vst3`) from a local file, adds
  it as a machine, and plays it from patterns and from live input;
- sequenced notes and parameter changes are sample-accurate, and live input has
  bounded latency (target ≤ 30 ms on top of the audio device);
- parameters are buzz globals and controller endpoints, and plugin state
  round-trips through `.bzw`;
- with engine v2, a Windows effect stays sample-aligned with the dry signal
  around it, and a sidechain plugin gets its key signal on its own input;
- **Dexed 0.9.3 plays dense 8-voice material at 48 kHz with no underruns** on a
  mid-range laptop for 10 minutes.

Out of scope: 64-bit plugins (see option 3 in performance.md), plugin editors in
v1, copy-protected plugins. Sidechains and multi-bus plugins come with engine v2
(§3.7).

## 2. Facts this design rests on

Measured in this repository (headless Chromium, Boxedwine single-threaded `jit`
build):

| Fact | Value |
|---|---|
| Emulator boot until the host is ready | ~20 s (with the wineboot fix) |
| First plugin start-up in a session | Dexed `VSTPluginMain`: ~9 s; later loads: ~0.5 s |
| Dexed audio processing | **3.8–4.7× realtime**, from the first render |
| PoC Synth audio processing | 12–20× realtime |
| Emulated vs. native DSP | ~100–200× slower |

From the code:

- **buzz-remote** renders the whole machine graph synchronously inside one
  AudioWorklet `process()`. Machines implement `ProcessMachine.process()` and get
  sample-offset events from `WorkletTimelineScheduler`, which is driven tick by
  tick (`onTickStart`). There is no latency or plugin-delay-compensation concept
  yet. `engine/telemetry/audioRing.ts` already provides a SharedArrayBuffer audio
  ring, and the prometheos host serves COOP/COEP (`require-corp`).
- **Boxedwine** devices are `FsVirtualOpenNode` subclasses registered with
  `Fs::addVirtualFile("/dev/…")` (`source/kernel/devs/`, `source/sdl/startupArgs.cpp`).
  Its browser audio already uses a SharedArrayBuffer PCM ring with an `Int32Array`
  control block feeding an AudioWorklet (`boxedwine-audio-worklet.js`).
- In Boxedwine's **multithreaded** builds (`-pthread -sPROXY_TO_PTHREAD`), guest
  threads run on pthreads (Web Workers), the WebAssembly memory is a
  SharedArrayBuffer, and JavaScript called from a pthread is **proxied to the
  browser main thread** (`boxedwine-multithreaded-audio.js`).
- Wine maps drive `Z:` to `/`, so a Windows program can open a Boxedwine
  device as `Z:\dev\<name>`.
- The WebCLAP host has no `clap.latency` support and writes 0 into each audio
  buffer's `latency` field (`WclapPlugin.ts`).

**Engine v2** ([its spec](https://github.com/dahlgrenmartin/prometheos-apps/blob/main/apps/buzz-remote/docs/superpowers/specs/2026-10-04-engine-v2-ports-sidechain-design.md)), from the Buzz beta machine
interface:

- per-machine latency (`GetLatency` → `MachineInstance.latency()`), polled by the
  worklet, with per-edge delay compensation that re-aligns when a latency changes;
- multiple named stereo inputs and outputs per machine, connections that land on
  a specific input, and `NULL` for an unconnected input or a silent output
  (`MIF_MULTI_IO`, `MultiWork`); the WebCLAP mapping is one stereo CLAP port per
  channel, the first input main, the rest sidechains;
- stereo-in/stereo-out effects (`MIF_STEREO_EFFECT`);
- work calls of at most 256 frames (`MAX_BUFFER_LENGTH`);
- a native dynamics compressor with a sidechain input.

## 3. Brainstorm: options per decision

### 3.1 Where the emulator runs

| Option | Verdict |
|---|---|
| Single-threaded build on the main thread (today's PoC) | **Offline only.** React and UI work would starve it and cause underruns; keep it for bouncing/freezing. |
| **Multithreaded JIT build in a hidden same-origin iframe; guest threads on pthreads** | **Chosen.** Emulation leaves the main thread, and one guest thread per plugin instance can run in parallel. Its single-thread speed against the `jit` build is unmeasured (Phase 0, task 1). |
| Whole emulator inside the AudioWorklet | Rejected: `process()` must return within one render quantum; the emulator can't be pre-empted, fetch files or start threads there. |

### 3.2 Transport between the guest and buzz-remote

| Option | Verdict |
|---|---|
| Files and the mailbox (today's PoC) | Rejected for real time: polling, Boxedwine's directory cache, filesystem overhead. |
| Windows audio (`waveOut`/DirectSound) through Boxedwine's audio ring | Rejected: output only (no MIDI or parameters in), one mixed stream, resampled (shell default 11,025 Hz), no per-instance routing. |
| TCP/WebSocket through Boxedwine's networking | Rejected: protocol stacks on the hot path, unbounded jitter. |
| **New device `/dev/vstbridge` with its rings in shared WebAssembly memory** | **Chosen.** The guest uses plain `ReadFile`/`WriteFile`; the emulator thread and the AudioWorklet exchange blocks with `Atomics` on the same memory: no copies through JavaScript, no main thread. This is yabridge's shared-memory audio path in emulator form. |
| A custom syscall (`int 0x80` with an unused number, called from Windows code) | Fallback if Wine mishandles a character device; less debuggable. |

### 3.3 Latency strategy

The machine's latency is the bridge's `L` plus the plugin's own (VST2
`AEffect.initialDelay`, VST3 `IAudioProcessor::getLatencySamples()`).

| Option | Verdict |
|---|---|
| Fixed, uncompensated `L` | **Only before engine v2.** Fine for a lone instrument; wrong for an effect next to its dry signal or on a send, which comes out `L` late. |
| **Report the latency through `MachineInstance.latency()` and let the engine compensate** | **Chosen.** The engine delays parallel paths so they meet aligned. This is what makes Windows *effects* usable, and it is the same mechanism a latent CLAP plugin needs. No compensation code in the winvst machine. |
| **Pre-roll sources**: for a machine without audio inputs, the engine delivers its sequenced events `latency` samples early instead of delaying everything else | **Ask of engine v2** (this replaces this design's earlier winvst-only scheduler change). Pattern playback through a Windows instrument then adds no latency to the song. Machines that take audio input still need delay-based compensation; live notes and knob moves on the machine still pay `L`. |
| **Freeze**: render a heavy plugin's track ahead, in the background, at any speed, and play the cached audio | **Later, the safety net** for plugins that can't keep up in real time (a DAW "freeze track" for emulated plugins). Re-render the affected span when patterns change. |

Latency changes only on a restart, never mid-playback, as CLAP requires
(`clap_host_latency.changed` with the plugin deactivated). That covers both a
higher `L` after underruns and a plugin that changes its own latency (VST2
`audioMasterIOChanged`, VST3 `restartComponent(kLatencyChanged)`).

### 3.4 Process model

| Option | Verdict |
|---|---|
| One `vsthost` process per plugin instance (yabridge's default) | Rejected: each Wine process costs start-up time and memory. |
| **One `vsthost` process, one guest thread per instance** (yabridge "plugin groups") | **Chosen.** One Wine boot; the MT build maps guest threads to pthreads, so instances run in parallel. |

### 3.5 Block size

The emulator renders blocks of `B` frames. Bigger blocks amortize per-block
overhead (device call, wake-up, JIT dispatch) but raise latency. Default
`B = 256` (the same as `MAX_BUFFER_LENGTH`, so one engine work call never spans
more than one block), configurable 128–1024. `L` defaults to `4B` (1,024
frames ≈ 21 ms at 48 kHz). After underruns the machine proposes a higher `L`
and applies it at the next stop, because the new latency re-aligns the graph.

### 3.6 Start-up cost

- Boot the emulator lazily: on the first Windows machine, or when a project
  that contains one is opened, with visible progress.
- Persist the Wine prefix in IndexedDB (`storage=idb`) so first-run work happens
  once per browser, not once per visit.
- Ship a slimmer Wine filesystem (Boxedwine documents building one for the
  web); 158 MB today.
- Keep plugins loaded across graph rebuilds; the instance's lifetime belongs to
  the emulator host, not to the worklet's machine object.
- Warm up after load: render a short note burst to silence so first-time JIT
  translation of the note path doesn't hit live playback.
- Idea to research: snapshot the booted emulator's memory and restore it (as v86
  does) for near-instant start.

### 3.7 Ports: stereo effects, multi-IO and sidechains

| Option | Verdict |
|---|---|
| One stereo input and output only | Only before engine v2. |
| **Each plugin bus becomes one engine v2 stereo channel** | **Chosen**, mirroring the WebCLAP mapping. **VST3:** every audio bus from `getBusInfo` (`kMain`/`kAux`, name), arranged as stereo with `setBusArrangements`; the main input is the main input, and aux inputs are sidechains. **VST2:** `numInputs`/`numOutputs` taken in pairs and named with `effGetInputProperties`/`effGetOutputProperties`; input pairs after the first count as sidechains, the usual VST2 convention (a 4-input compressor). Mono buses are up- or down-mixed at the edge. |
| Unconnected inputs | Engine v2 passes `NULL`. The worklet sets a "connected" bitmask in the request; the guest feeds silence and sets VST3 `silenceFlags`. Buses stay active, because VST3 only allows `activateBus` while the plugin is inactive. |

The port list is part of `DESCRIBE` and is frozen at install, like WebCLAP
descriptors, so the patch view and connection routing are shared with
WebCLAP machines. More ports cost little: two stereo inputs at 256 frames are
4 KB per block, against hundreds of microseconds of emulated DSP.

## 4. Architecture

```text
main thread (buzz-remote UI)                        AudioWorklet (buzz engine)
  WinVstHost ──────── creates ───────────┐           WinVstMachine (ProcessMachine)
   │ boots hidden iframe (Boxedwine MT)   │            │ per block: write request,
   │ sends LOAD / DESCRIBE / STATE        │            │ Atomics.notify, read output
   │ watchdog, restart                    ▼            │ L frames behind
   │                     ┌──── shared WebAssembly memory (SharedArrayBuffer) ────┐
   │                     │  vstbridge region: header + one channel per instance  │
   │                     │  request ring (events, transport) │ audio-out │ audio-in │
   │                     └──────────────────▲─────────────────────────────────────┘
   ▼                                        │ read/write via /dev/vstbridge
Boxedwine MT (pthreads)  ── Wine ── vsthost.exe --bridge
                                         └─ one guest thread per plugin instance:
                                            ReadFile(request) → plugin process → WriteFile(audio)
```

## 5. Components

### 5.1 Boxedwine: `/dev/vstbridge` (patch `0003-vstbridge-device.patch`)

- `source/kernel/devs/devvstbridge.cpp`, registered in `startupArgs.cpp` next
  to `/dev/null`. One open file handle is bound to one channel.
- **Region** allocated once at start-up in WebAssembly memory (before any
  growth); an exported `_vstbridge_region()` returns its address and size.
  JavaScript re-acquires `HEAPU8.buffer` after memory growth; offsets stay
  valid.
- **Channel layout** (little-endian, 64-byte aligned, versioned header):
  - control words (`Int32`): state, sample rate, block frames, `requestSeq`,
    `responseSeq`, underruns, last process time (µs), fault code;
  - port counts (stereo inputs, stereo outputs), fixed at `LOAD`;
  - request ring: fixed-size records, one per block: `blockIndex`, frame count,
    the input-connected bitmask, transport (playing, tempo, beat position), then
    up to N events `{offset, type, a, b, value}` (note on/off, CC, pitch bend,
    parameter, program);
  - one audio ring per stereo port, inputs and outputs: planar float32,
    power-of-two capacity.
- **Guest calls**:
  - `ioctl(ATTACH, channel)` binds the handle; Wine's `DeviceIoControl` or a
    first `WriteFile` command carries it (Phase 0 decides which works through Wine).
  - `ReadFile` blocks until `requestSeq` advances, then returns the request
    record. Blocking uses the kernel's condition mechanism; in the MT build the
    emulator thread parks with `emscripten_futex_wait` on the `requestSeq` word,
    so the worklet's `Atomics.notify` wakes it **with no main-thread hop**.
  - `WriteFile` copies one block of output into audio-out and advances `responseSeq`.
- Single-threaded build: the same device, non-blocking (polling); used for
  offline rendering only.

### 5.2 `vsthost --bridge`

- Opens `Z:\dev\vstbridge` (control channel 0) and serves commands from the
  main thread: `LOAD(channel, path, rate, block)`, `DESCRIBE`, `GET_STATE`,
  `SET_STATE`, `UNLOAD`, `PING`.
- `LOAD` starts one guest thread per instance. That thread opens its channel,
  then loops: `ReadFile` request → apply events at their offsets → `process` B
  frames → `WriteFile` output. It is a strict request/response loop with no
  timing of its own.
- Per-instance thread setup: FTZ/DAZ in MXCSR and x87 control word, so
  denormals can't stall the emulated FPU; `SetThreadPriority(TIME_CRITICAL)`.
- `DESCRIBE` returns the JSON the offline host already produces (name, vendor,
  kind, parameters with display text) plus the port list (§3.7) and the
  plugin's latency, used once at install time.
- Latency or I/O changes reported by the plugin go back on the reply ring; the
  machine applies them at the next restart (§3.3).
- State: VST2 `effGetChunk`/`effSetChunk` (or the parameter list when the
  plugin has no chunks); VST3 `IComponent::getState`/`setState` and the
  controller's `setComponentState`, through an in-memory `IBStream`.

### 5.3 buzz-remote

- **Format** `winvst` next to `webvst` and `webclap` in `src/engine/plugins`:
  the archive holds the DLL or `.vst3` bundle plus a host-generated
  `winvst.json` (from `DESCRIBE`, frozen at install like WebCLAP descriptors).
  Class id: `winvst:<sha256-of-binary>:<name>`.
- **`WinVstHost`** (main thread, `src/engine/winvst/`): owns the hidden iframe
  with the MT Boxedwine build, boot progress, channel allocation and the
  control channel; posts the region's SharedArrayBuffer plus channel offsets to
  the worklet; runs the watchdog.
- **`WinVstMachine`** (worklet), written against the engine v2 machine
  interface: multi-port work call, latency getter.
  - Each block: write this block's inputs and events into the next request,
    `Atomics.notify`, and read the outputs written `L` frames earlier. On a
    shortfall, output silence, count an underrun and report it through
    telemetry. No allocation in the work call.
  - Reports `L` + plugin latency. Compensation and source pre-roll belong to
    the engine (§3.3); seek, loop and tempo changes flush the rings and
    re-sync.
  - Before engine v2 lands: the current `ProcessMachine`, main stereo ports
    only, uncompensated `L`.
- Parameters as `word` globals (`endpointKey = winvst:<index>`), the same
  encoding as the WebCLAP backend; plugin-side changes come back on a reply ring.
- Persistence: `prometheos.winvst/1` project extension, with `Machine.data` =
  plugin state, mirroring `prometheos.webclap/1`.
- UI: generic parameter window. **Editors later, almost for free:** Boxedwine
  already draws Wine windows to its canvas and forwards mouse input, so
  `effEditOpen`/`IPlugView::attached` on a `vsthost` window can show the real
  plugin GUI in a buzz editor window that hosts the emulator canvas.

## 6. Real-time budget

- **Throughput:** each instance's DSP must finish a block in under `B / rate`
  with headroom. Dexed uses ~25 % of one emulator thread at 44.1 kHz in the ST
  build. With one guest thread per instance, instances are bounded per thread,
  not in total, up to the number of cores.
- **Per-block overhead target:** < 0.3 ms (device calls, ring copy, futex
  wake-up). Phase 0 measures it.
- **Jitter sources:** first-time JIT translation of new code paths (addressed
  by the warm-up), Wine's background threads competing in Boxedwine's
  scheduler, and memory-growth stalls (prevented by allocating up front).
  Proxied JavaScript is designed out of the hot path.
- **Fallbacks when the budget is exceeded:** raise `L`; switch the machine to
  freeze mode (§3.3); show the measured realtime factor in the machine's info
  so users can choose.

## 7. Risks and open questions

| Risk | Plan |
|---|---|
| Boxedwine's MT JIT build stability with Wine and JUCE threads (only the ST build was tested here) | Phase 0 gate: Dexed loads and renders in the MT build. |
| Boxedwine's README lists "multi-threaded build also stutters with sound" as a known issue | The cause is not documented. If it is the `waveOut` path (proxied through the main thread), `/dev/vstbridge` bypasses it; if it is guest scheduling, it hits the bridge too. Phase 0 measures block jitter directly. |
| Wine opening `Z:\dev\vstbridge` as a character device; blocking `ReadFile`; `DeviceIoControl` reaching the device's `ioctl` | Phase 0 test program; fallback is the custom syscall. |
| `emscripten_futex_wait` from the emulator thread on a word that the worklet notifies | Phase 0 micro-benchmark of wake-up latency. |
| Memory growth replacing `HEAPU8.buffer` | Allocate the region at start-up; consumers re-acquire views on growth. |
| Underruns from JIT spikes and scheduling | Warm-up render, adaptive `L`, freeze fallback, telemetry. |
| 158 MB first download | IndexedDB prefix plus a slim filesystem (Phase 3). |
| Phase 1 depends on engine v2's latency and port interface | Build against it; if it is late, ship with the current `ProcessMachine`, main ports and uncompensated `L`, and switch over without changing the bridge. |
| Latency changes (a higher `L`, a plugin's own latency) | Engine v2 re-aligns whenever `latency()` changes, but a new `L` also re-syncs the bridge rings, so the machine changes it only at stop/restart. |
| Licences | Users supply their own binaries; Boxedwine GPL-2.0+, Wine LGPL, host GPL-3.0. |

## 8. Testing

- **Layout:** one shared description of the region, checked from C++
  (`offsetof`) and TypeScript, like the CLAP ABI golden test in buzz-remote.
- **Protocol:** ring wrap-around, sequence numbers, event encoding,
  underrun/recovery, all in TypeScript unit tests against a fake guest.
- **Correctness:** the real-time output, shifted by `L`, must be
  **sample-identical** to the offline render of the same events (the existing
  `vsthost` one-shot mode is the reference). This catches lost, late or
  misplaced events.
- **Ports (engine v2):** a PoC sidechain plugin, built here as VST2 (4
  inputs) and VST3 (main plus aux bus), that outputs main × sidechain (the
  "product" output of the beta SDK's `modulator.cpp`). Run through the
  sample-identity check, it proves per-port routing and `NULL` inputs.
- **Latency compensation (engine v2):** a PoC invert effect in parallel with
  its dry signal; with correct compensation the mix is exactly silent, which
  proves alignment to the sample.
- **Endurance (headless Chromium):** Dexed, 10 minutes of sequenced 8-voice
  material, zero underruns at the default `L`; record block-time percentiles.
- **buzz-remote:** install, describe, `.bzw` round trip, controller routing,
  graph rebuild without plugin reload.

## 9. Phases

0. **Spike and go/no-go** (in `experiments/boxedwine-vst`): MT build plus `/dev/vstbridge`
   plus `vsthost --bridge`, one channel, a test page with an AudioWorklet and an
   on-screen keyboard playing Dexed live. The shared layout carries port counts
   and per-port rings from the start, exercised with one stereo output. Measure the realtime factor in MT,
   wake-up latency, block jitter and underruns at `L` = 512/1,024/2,048.
1. **buzz-remote machine on engine v2:** format, install/describe, ports and
   sidechains, reported latency, parameters, state, `.bzw`, telemetry. Engine
   v2's compensation and source pre-roll replace this design's earlier
   winvst-only render-ahead.
2. **Freeze** mode.
3. **Scale and polish:** parallel instances, plugin editors through the
   Boxedwine canvas, IndexedDB prefix, slim filesystem, start-up snapshot
   research.

## 10. Asks of engine v2

What the winvst machine needs from the engine milestone (all of it is also
useful for latent or multi-port WebCLAP plugins):

1. A per-machine latency that the engine compensates by delaying parallel
   paths. Covered by `MachineInstance.latency()` and the engine v2 delay
   compensation.
2. Not covered yet: pre-roll for machines without audio inputs: sequenced events delivered
   `latency` samples early, so latent instruments add no latency to playback.
3. Named stereo ports with a main or sidechain role, frozen with the machine's
   descriptor, `NULL` for unconnected inputs, and connections that name the
   target port.
4. Per-machine latency and underrun counts in telemetry, so the UI can show why
   a machine is late or glitching.

## 11. Phase 0 outcome and deviations

Phase 0 ran in `experiments/boxedwine-vst`; numbers in its
[results.md](../results.md#real-time-phase-0-dexed-streamed-live-into-an-audioworklet).
**Go**: Dexed streamed sequenced 8-voice chords at 48 kHz for 10 minutes with
zero underruns at L = 2,048 frames, and its live output, shifted by L, is
bit-identical to the offline replay.

Where the implementation departs from §3-§9:

| Design | Built | Why |
|---|---|---|
| Two Boxedwine patches plus 0003 | Plus `0004-emscripten6-pthread-worker-commands.patch` | The MT build hung under Emscripten 6 (its module broker used removed PThread internals and string worker commands). |
| §3.1 MT build "~50 % faster" (Boxedwine docs) | About as fast per thread as the ST build (Dexed 4.7-5.8x vs 5.2-5.4x) | Measured; the gain is one thread per instance off the main thread, not per-thread speed. |
| §5.1 ATTACH via `DeviceIoControl` or a first write | First `WriteFile` (`vstb_attach`, which also carries B, ports and the plugin latency) | Wine answers `DeviceIoControl` on a unix device with `ERROR_NOT_SUPPORTED`. |
| §5.1 guest blocks on `requestSeq` | Blocks on a per-channel `doorbell` futex word, bumped with every request and every **kick** | A kick wakes an instance thread that waits for audio so commands (describe, state, unload) run on the plugin's own thread, serialized with `process`. |
| §5.1 worklet reads once `responseSeq * B` covers the frames | Reads block k once `doneBlock[k % SLOTS] == k + 1` | The device may skip blocks a late guest can no longer read intact; `responseSeq` would then pass stale ring data. |
| §5.1 last process time from the guest | Per-block `turnUs` and histograms measured by the device | The guest clock in Boxedwine moves in 1 ms steps. |
| §5.2 commands LOAD/DESCRIBE/GET_STATE/SET_STATE/UNLOAD/PING | Plus `PUT_FILE` | Files the page writes into Emscripten's FS are invisible to Wine (directory cache), so a user's plugin binary is uploaded through the bridge after boot. |
| §3.5 L defaults to 4B = 1,024 frames | **L = 2,048** (42.7 ms) | In 10 minutes of Dexed: L = 2,048 none, L = 1,024 80 underrun blocks (rare 8-21 ms stalls), L = 512 23,177. B stays 256. |
| §3.6 warm-up "a short note burst" | 3 s of 8-note chords across the keyboard, then a suspend/resume | A 1 s single chord left first-time JIT translation in the live path (14 underruns in the first 1.75 s). |
| §5.2 parameter events at their offsets | VST2 parameters apply at the block start; MIDI keeps its offset | VST2 has no sample-accurate parameter API; the offline replay does the same, so identity holds. |
| §8 offline reference = vsthost one-shot mode | `vsthost --replay` of the captured request stream through the bridge's own instance code, in a separate ST-build session | The one-shot mode renders a fixed chord; replaying the exact requests (events, block size, warm-up) is what makes bit identity a test of the bridge. |

Phase 1 (buzz-remote) builds on this: `src/engine/winvst/` holds the TypeScript
twin of the layout (tested against the same golden JSON), `WinVstMachine`
(worklet), `WinVstHost` and its coordinator (main thread), the `winvst` package
format and `prometheos.winvst/1`. Engine v2's delay compensation is used as is
(`latency()` = L + `initialDelay`); source pre-roll (§10, ask 2) is still open,
so a Windows instrument plays L late against the rest of a song.

### Phase 1 outcome and deviations

Verified in headless Chromium by `tests/winvst-browser` (numbers in the
experiment's
[results.md](../results.md#phase-1-windows-vst-machines-in-buzz-remote)):
Dexed as a winvst machine in a buzz-remote song, 10 minutes of sequenced
8-voice chords at L = 2,048; identity with `vsthost --replay` for Dexed and the
PoC Synth; the dry + PoC Invert null; the `.bzw` round trip of Dexed's chunk.

| Design | Built | Why |
|---|---|---|
| §8 buzz-remote checks in the app | `tests/winvst-browser`: a page running buzz-remote's real worklet bundle, `WinVstCoordinator`/`WinVstHost` with the emulator iframe, the package install (DESCRIBE), the project codec and project bridge; it sends the worklet what `EngineBridge` sends. `EngineBridge`, React and the app shell are not loaded | The app depends on the private `@prometheos/shared`, which this environment cannot fetch, so the app itself was neither built nor run here. The harness covers the same engine and persistence code paths. |
| §8 identity against the stream start | The engine's master output is recorded by a recorder node; `identity.mjs --align 1` finds where the stream starts in the recording (of the shifts that reproduce the loudest reference block, the one matching the most blocks) | A recording of the engine does not begin with the machine's stream; a looping song repeats blocks bit for bit, so one block match is not unique. |
| §8 "the mix is exactly silent" | Right channel bit-exact silence; the left keeps at most \|x\| · 2^-52 | The pan law's centre gains are 1 (right) and 1 + 2^-52 (left, sqrt2 · cos(pi/4)), and a delayed edge mixes as `target + src * gain` in double before the float32 store, so -x + x·g leaves x·2^-52 on the left. Compensation itself is exact: without it the mix peaks at 1.03. |
| §3.6 warm-up | 7 s: the 3 s of legato chords, then detached chords held 0.2 s with 0.8 s of silence, transport playing | The first chord after voices had fully decayed took a 15-50 ms turn live (3 underrun blocks at L = 2,048 in one of two runs). |
| §5.2 UNLOAD frees the plugin | The module stays loaded (no `FreeLibrary`); a later instance shares it | Unloading Dexed for the third time made Boxedwine's MT JIT panic (`KMemory::commitPreparedCodeInvalidation`) and ended the emulator. Reusing the translated code also cuts Dexed's LOAD from 2.9 s to 2.2 s. |
| Brief: WinVstHost watchdog | A channel more than SLOTS blocks behind for 5 s silences its machines, tears the emulator down and reports them; the next attach boots afresh. No automatic restart | A Boxedwine panic ends every thread (its message box `alert()` does not exist in a pthread worker, so the panic text is lost unless the build is patched). Restarting blindly could loop. |

Not in the MVP: the PoC sidechain plugin and multi-port routing tests (§8),
source pre-roll, Freeze, plugin editors.

## 12. The WebCLAP split (option A)

Boxedwine is GPL, so the emulator and everything built around it moved out of
prometheos-apps into this repository, and buzz-remote plays Windows plugins as
WebCLAPs it loads dynamically. buzz-remote keeps only a generic host feature,
`prometheos.runtime/1`: a WebCLAP names a runtime page, and the host loads it
once and hands it the module's shared memory. Its design is in buzz-remote's
`docs/superpowers/specs/2026-10-04-webclap-runtime-pages.md`.

Two ways to run the runtime were weighed:

- **A (chosen):** a trusted, same-origin runtime page that shares the WebCLAP
  module's memory with the AudioWorklet. The audio path stays on shared memory
  and futexes, as in Phase 0/1. The page runs with the host's origin, so hosts
  only load runtimes they trust, and the host serves them itself.
- **B:** a sandboxed runtime (origin `null`). It cannot share memory with the
  app (cross-origin isolated pages key agent clusters by origin), so every
  block would cross by messages between workers, with event-loop jitter.

| Phase 1 (in-tree winvst machine) | WebCLAP (this repository) | Why |
|---|---|---|
| `WinVstMachine` (TypeScript, in the worklet) reads Boxedwine's memory | `wclap/vstloader.c`: a WebCLAP whose channel lives in its own shared memory; `runtime/relay-worker.js` copies each request and each answered block between that channel and `/dev/vstbridge` (2 workers per instance, futex waits) | A WebAssembly module cannot address a second memory, and the plugin's code must not live in buzz-remote |
| `winvst` package, descriptor frozen at install by DESCRIBE | A `.wclap` bundle with `resources/vstloader.txt`, frozen when the `.dll` is wrapped (`wrap/wrap.mjs`) | buzz-remote installs it like any WebCLAP |
| `WinVstHost` and its coordinator in buzz-remote | `runtime/` (this repository) plus the host's `PluginRuntimeHost` | Same split |
| `prometheos.winvst/1` project extension; state read at save time | The WebCLAP package and `clap.state`. The state is the runtime's last report, refreshed at most every 0.5 s after parameter changes | `clap.state.save` is synchronous; the chunk lives in the emulator |
| `WinVstHost`'s watchdog | Not ported yet | Next step |

Measured through buzz-remote's WebCLAP path (results.md, "The WebCLAP in
buzz-remote"): Dexed for 10 minutes with 0 underruns at L = 2,048; output
bit-identical to `vsthost --replay` (Dexed 3,739/3,739 and PoC Synth
3,748/3,748 blocks); the invert null and the state round trip pass, as they
did in Phase 1.
