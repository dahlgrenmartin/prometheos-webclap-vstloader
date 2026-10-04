# Results

Offline measurements: 2026-10-04 with Boxedwine `509f6a7` plus patches 0001 and 0002 in
`patches/boxedwine/`, the `jit` Emscripten target (Emscripten 6.0.11), the Wine
11 filesystem `TinyCore15Wine11.0.zip` (SHA-256 `e38234f9…6b79`) and headless
Chromium (Playwright). Every render is a 3-note chord (MIDI 60, 64, 67) at
44.1 kHz with a note-off at 60 % of the length.

## In the browser

`tests/browser_render.mjs` against `dist/`, one page session, one emulator boot:

| Plugin | Format | Origin | Peak | RMS | Params | Audio processing (2 s of audio) | Plugin start-up |
|---|---|---|---|---|---|---|---|
| PoC Synth | VST2 | built here (MinGW) | 0.419 | 0.118 | 3 | 97 ms (**~20× realtime**) | 0.2 s |
| PoC Synth | VST3 | built here (MinGW) | 0.419 | 0.118 | 3 | 167 ms (**~12× realtime**) | 0 s |
| Dexed 0.9.3 | VST2 | asb2m10, JUCE, MSVC, 2017 | 0.351 | 0.063 | 155 | 430–530 ms (**3.8–4.7× realtime**) | 9.1 s first time, then 0.55 s |

Start-up is a one-off cost per load: the first `VSTPluginMain` in a session
runs JUCE's and Wine's GUI start-up through a cold JIT. In this
render-per-job harness, each job also pays `LoadLibrary` (0.5 s) and Dexed's
`FreeLibrary` (6–8 s while JUCE stops its threads); a real-time host would load
the plugin once. See [performance.md](performance.md).

- Emulator boot until `vsthost --serve` is ready: **20 s** (from page load, with
  the Wine zip served locally).
- No non-finite samples in any render.
- The VST2 and VST3 builds of the test synth produce identical statistics, so
  both ABIs deliver the same MIDI and audio.
- `--play` (Windows `waveOut`) plays the render through Boxedwine's browser
  audio backend: `waveOut: played 44100 frames`.

![Dexed rendered in the browser](dexed-in-browser.png)

## Real time (Phase 0): Dexed streamed live into an AudioWorklet

Measured on 2026-10-04 in headless Chromium (Playwright, 4 vCPUs, no audio
hardware: Chromium's fake output still runs the AudioContext in real time) with
Boxedwine `509f6a7` plus patches 0001-0004, the **`multiThreadedJit`** target
(Emscripten 6.0.11), `vsthost --bridge` and `web/realtime.html`. Material:
`ChordSequencer` in `web/realtime-worklet.js`, a new 8-note chord every 0.5 s
(off the block grid, varied velocity), held 0.4 s: 8 voices plus release tails.
48 kHz, block B = 256 frames (5.33 ms budget per block). Plan:
[Phase 0](../../../apps/buzz-remote/docs/superpowers/plans/2026-10-04-realtime-windows-plugins-phase0.md).

### Gate 1: the multithreaded build (task 1)

`tests/browser_render.mjs` (offline harness), same machine, ST vs MT:

| | ST `jit` | MT `multiThreadedJit` |
|---|---|---|
| Boot to a serving vsthost | 19.0 s | 30.5 s |
| PoC Synth VST2, 2 s chord | 23.5x realtime | 18.7x |
| PoC Synth VST3 | 13.5x | 10.6x |
| Dexed, first render (incl. VSTPluginMain) | 8.4 s, 5.2x | 10.0 s, 5.8x |
| Dexed, later render | 5.4x | 4.7x |
| Dexed output | peak 0.351193, RMS 0.062909 | identical |

The MT build first hung before `main()`: its WASM JIT module broker read
`PThread.runningWorkers`, gone in Emscripten 6, and posted the string worker
command `'callHandler'`, which Emscripten 6 drops (it numbers worker commands).
Patch `0004-emscripten6-pthread-worker-commands.patch` fixes both (boot went from
never, to 145 s with modules no longer shared between workers, to 30 s). Per
thread, the MT JIT is about as fast as the ST one, not the ~50 % faster its docs
suggest; its gain is that each plugin instance gets its own thread off the main
thread. **Gate: passed** (Dexed renders correctly in the MT build).

### Device reachability through Wine (task 2)

`tests/devtest.c` under Wine in the MT build:

| Probe | Result |
|---|---|
| `CreateFileA("Z:\dev\null")` | ok, `GetFileType` = char device |
| `ReadFile` / `WriteFile` | reach the device: 26 us / 16 us per call |
| `DeviceIoControl` | fails, `ERROR_NOT_SUPPORTED` (Wine does not pass ioctls to a unix device) |
| Linux `int 0x80` from Windows code | `open`/`read`/`write` work, 2 us per call |

So the device is reached with plain `ReadFile`/`WriteFile`, and a handle is bound
to a channel by its first write (the spec's fallback for ATTACH).

### Wake-up and per-block overhead (task 6)

`web/bench-worker.js`: a dedicated worker publishes one request at a time on a
channel running the PoC Synth (2,000 blocks after 200 warm-up blocks) with its
publish time in the request; the device histograms publish -> guest wake.

| | p50 | p90 | p99 | max |
|---|---|---|---|---|
| Wake-up (publish -> emulator thread returns from `futex_wait`) | 8-16 us | 16-32 us | 64-128 us | 2-4 ms |
| Round trip (publish -> response visible), PoC Synth 256 frames | 0.48 ms | 0.58 ms | 0.83 ms | 6.8 ms |

The PoC Synth's own processing is about 0.28 ms per block (19x realtime), so
device calls, Wine's `ReadFile`/`WriteFile`, the copies and both wake-ups add
about **0.2 ms per block**, inside the spec's 0.3 ms target. No main-thread hop
is involved: the worklet's `Atomics.notify` wakes the emulator thread directly.

### Underruns over 10 minutes (task 6)

Dexed 0.9.3 (JUCE, 155 parameters), `tests/realtime.mjs --seconds 600`,
plugin loaded once and warmed up (`Vst2Instance::warmUp`: 3 s of 8-note chords).
Block time is the device's request-delivered -> response-written time (the guest
clock in Boxedwine has 1 ms steps, so the guest's own timing is too coarse).

| L | Streamed | Underrun blocks | Min margin | Block time p50 / p90 / p99 / p99.9 / max |
|---|---|---|---|---|
| **2,048 frames (42.7 ms)** | 600 s | **0** of 112,510 | 128 frames | 2.53 / 2.85 / 4.05 / 4.75 / 21.3 ms |
| 1,024 frames (21.3 ms) | 600 s | 80 (0.38 s of silence) | -2,048 frames | 2.53 / 2.83 / 4.08 / 4.80 / 15.7 ms |
| 512 frames (10.7 ms) | 600 s | 23,177 (63 s of silence) | -1,920 frames | 2.53 / 2.95 / 5.25 / 7.00 / 12.1 ms |

(The L = 512 run overlapped with package installs and test runs on the same
machine, which pushed its p99 up; it underran from its first second regardless:
its slack, L - B = 256 frames, is about one block's budget.) Run summaries:
`runs/dexed-L*-600s.json`.

With 8 voices Dexed needs 2.5 ms of a 5.33 ms block (2.1x realtime; 4.7x for the
offline harness's 3-note chord), and 99.2 % of blocks finish within 4.1 ms. Rare
stalls of 8-21 ms (13 of 112,516 blocks over 8 ms) are what L has to absorb: at
L = 2,048 the slack is L - B = 1,792 frames (37 ms). Without the warm-up, the first
1.75 s of live chords underran 14 times (first-time JIT translation); with it,
none.

![Dexed streamed for 10 minutes with no underruns](realtime-dexed-600s.png)

### Sample identity (task 6)

`tests/identity.mjs` takes the request stream the worklet published (15 s,
2,813 blocks of 8-voice chords) and the output it played, renders the same
requests offline with `vsthost --replay` in a separate session of the
**single-threaded** build, and compares the live output shifted by L with that
render bit for bit: **2,813 of 2,813 blocks identical** (peak 0.79,
`runs/identity-dexed-L1024.json`). An earlier capture with underruns matched in
every block that was not played as silence. The replay runs the same
`Vst2Instance` code (warm-up, events, block size) on a fresh instance, so live
streaming loses, delays or moves no event.

### Go / no-go

**Go.** Dexed streams with zero underruns at L = 2,048 frames for 10 minutes, and
its live output is sample-identical to the offline render. None of the no-go
triggers hold: Wine and JUCE run reliably in the MT build (with patch 0004);
wake-up jitter is microseconds and block stalls exceed a block only rarely (p99.9
4.75 ms < 5.33 ms); Dexed's MT realtime factor is 2.1x with 8 voices (above the
1.5x floor).

Carried into Phase 1: L = 2,048 by default (L = 1,024 underruns, see above); the
warm-up; one more 10-minute run per change to the hot path. Not measured here:
a real laptop browser with audio hardware (headless only in this environment).

## Phase 1: Windows VST machines in buzz-remote

Measured on 2026-10-04 in headless Chromium (Playwright, 4 vCPUs, fake audio
output) with the same emulator build as Phase 0 plus the Phase 1 `vsthost`
(7 s warm-up, modules kept loaded). The page is
`apps/buzz-remote/tests/winvst-browser` (`build.mjs`, then `run.mjs`): it runs
buzz-remote's real AudioWorklet bundle (`src/engine/buzz-worklet.ts`), the
`winvst` package install (DESCRIBE in the emulator, frozen descriptor),
`WinVstCoordinator`/`WinVstHost` with the emulator in a hidden iframe, the
project codec and the `prometheos.winvst/1` project bridge, and sends the
worklet the `graph`/`song`/`sequence`/`transport` messages `EngineBridge` sends.
`EngineBridge` and the React app are not loaded: the app imports the private
`@prometheos/shared`, which this environment cannot fetch (see the design
spec's §11).

Song: one machine on 8 tracks, a 16-row pattern of four 8-note chords (each
held 3 rows, varied velocities, note-offs on row 3 of 4) looping at 126 BPM,
4 TPB, 48 kHz; the machine to Master at amp 1, centre pan. B = 256, **L = 2,048**.
Play starts as soon as the machine is attached (no settle time).

| Check | Result |
|---|---|
| Emulator boot (page load to `vsthost --bridge` serving) | 31.7 s |
| Dexed install (upload + DESCRIBE on channel 8, first JIT) | 11.0 s; descriptor frozen in the package |
| Dexed machine attach (LOAD + 7 s warm-up, channel 1) | 2.4 s (3.1 s until the worklet has the channel) |
| **Dexed, 600 s of 8-voice chords** | **0 underrun blocks**, 0 skipped, no episode of the guest more than 4 blocks behind; 2 device turns over 10 ms (14.6, 14.8 ms) |
| **Dexed identity** (first 20 s: 3,750 blocks, master output vs `vsthost --replay` in the ST build) | **3,750 of 3,750 blocks bit-identical** (peak 0.79) |
| PoC Synth, 62 s | 0 underrun blocks, no device turn over 10 ms |
| **PoC Synth identity** (20 s, 3,750 blocks) | **3,750 of 3,750 blocks bit-identical** (peak 0.89) |

The identity check records the engine's master output, so
`tests/identity.mjs --align 1` first finds where the machine's stream begins in
the recording (the shift that reproduces the loudest reference block and the
most blocks), then compares every block shifted by L bit for bit. Master's
own path (machine output x 32768, amp 1, centre-pan gains 1 + 2^-52 and 1,
/ 32768 into a float32 output) round-trips float32 samples exactly. Runs:
`runs/phase1-buzz-dexed-600s.json`, `runs/phase1-buzz-pocsynth-60s.json`,
`runs/phase1-identity-{dexed,pocsynth}.json`.

![Dexed as a winvst machine: 602 s, 0 underrun blocks](phase1-buzz-dexed-600s.png)

**Compensation (dry + PoC Invert null).** buzz-remote's FM synth (4 voices of
the same chord pattern) goes to Master directly and through the PoC Invert
(out = -in) as a winvst effect; the invert reports `latency()` = L = 2,048, and
engine v2 delays the dry edge by that much.

| 48 kHz, 10 s / 2 s recorded | Peak left / right | Non-zero samples left / right |
|---|---|---|
| Compensated (10 s, 480,000 frames) | 2.2e-16 / **0** | 387,153 / **0** |
| Dry path alone (invert muted) | 0.887 / 0.887 | 78,314 / 78,314 |
| Compensation off | 1.027 / 1.027 | 86,506 / 86,506 |

The right channel nulls bit for bit. The left keeps x * 2^-52: Buzz's pan law
gives the left channel 1 + 2^-52 at centre (sqrt2 * cos(pi/4)), and a delayed
edge is mixed as `target + src * gain` in double before the float32 store, so
-x + x * (1 + 2^-52) leaves x * 2^-52 (at most 2.2e-16 here, -313 dBFS). The
alignment itself is exact; without compensation the mix peaks at 1.03. The
invert had 0 underruns.

**.bzw round trip.** Dexed loaded as a machine, Cutoff and Resonance set
through the worklet (VST2 `setParameter`), its state captured (`effGetChunk`,
6,022 bytes) into `Machine.data` and saved portable (1.26 MB `.bzw`, the
binary embedded under `assets/plugins/`); then everything was released and
uninstalled and the project decoded and restored through
`prometheos.winvst/1` alone. The reopened machine's chunk is **byte-identical**
to the saved one, Cutoff and Resonance read back 0.187506 and 0.750023, and
no parameter differs from before the save.

**Checks.** buzz-remote: 26 winvst unit tests pass; the full vitest suite has
0 failing tests (2,013 passed; the base branch 1,987), and the same 50 test
files fail to import `@prometheos/shared` on this branch and its base; ESLint
0 errors (17 warnings, as on the base); `tsc --noEmit` the same 23 errors as
the base, all from the missing `@prometheos/shared`. The
`boxedwine-vst.yml` steps (`BUILD_SITE=0 ./build.sh`, the export checks,
`node --check web/app.js`, `node tests/abi_layout.mjs`) pass locally.

## Natively (Linux, Boxedwine x64 JIT)

| Plugin | Format | Result |
|---|---|---|
| PoC Synth | VST2 | peak 0.42, 50× realtime |
| PoC Synth | VST3 | peak 0.42, 16× realtime |
| Dexed 0.9.3 | VST2 | `LoadLibrary` succeeds; blocks in `VSTPluginMain` |

Native Dexed blocks because creating any window hangs this headless native
setup: `tests/wintest.c`, which only creates a hidden `STATIC` window, hangs the
same way under Xvfb, crashes Boxedwine with `-novideo` and exits silently with
SDL's dummy video driver. The browser build has a real canvas and Dexed works
there.

## Problems found and fixed on the way

| Symptom | Cause | Fix |
|---|---|---|
| Test synth renders exact silence in the browser; `exp`/`pow` return `1.0` (`tests/fputest.c`) | Boxedwine's shared FPU code ignored the x87 chop rounding mode in `FRNDINT`, breaking the `fldcw`/`frndint`/`f2xm1`/`fscale` idiom of x87 `exp`/`pow`. The native x64 JIT rounds correctly, which hid the bug natively for hot code. | `patches/boxedwine/0001-fpu-frndint-honours-chop.patch` |
| Every browser start waits ~300 s (`run_wineboot boot event wait timed out`) | The zip's prefix timestamp never matches, so Wine reruns `wineboot -u` on every fresh in-memory prefix, and in the browser that update never signals completion. | `vstpoc-prefix.zip`, layered first (`root=`), holds `.update-timestamp` = `disable` |
| Jobs dropped into the guest's directory never run | Boxedwine caches directory listings; files created by the page through Emscripten's FS are invisible to `FindFirstFile`. | Mailbox protocol: the guest creates `inbox.txt` and the page only rewrites its contents. |
| Nothing written to `D:` | `-mount_drive` did not produce a usable drive in this build (natively either). | Outputs go to `C:\vstpoc-out`, in the Wine prefix. |
| Link fails with undefined `operator new` | Emscripten 6 does not add libc++ to an `emcc` link. | `patches/boxedwine/0002-emscripten-link-with-em++.patch` |
| The multithreaded build never reaches `main()` (`Cannot read properties of undefined (reading 'concat')` in `preRun`) | Emscripten 6 removed `PThread.runningWorkers`, which Boxedwine's WASM JIT module broker reads | `0004-emscripten6-pthread-worker-commands.patch` |
| MT boot takes 145 s and logs thousands of `worker sent an unknown command callHandler` | Emscripten 6 numbers its worker commands (`CMD_CALL_HANDLER` = 9); the broker (and `sdlgl.cpp`) post the old string, so JIT modules are never shared between workers | same patch: the numeric command; boot 30 s |
| `DeviceIoControl` on `Z:\dev\vstbridge` fails (`ERROR_NOT_SUPPORTED`) | Wine does not forward ioctls to a unix character device | a handle is bound to its channel by its first `WriteFile` (`vstb_attach`) |
| Per-block guest timings are 0, 1000 or 2000 us | `QueryPerformanceCounter` under Boxedwine advances in 1 ms steps | block times come from the device (`emscripten_get_now`, us), `turnUs` per block |
| A guest that skipped blocks could have them played from stale ring data | `responseSeq` alone jumps past skipped blocks | the device stamps `doneBlock[k % SLOTS] = k + 1` after writing a block; the worklet plays only stamped blocks |
| The worklet never registers its processor | the shared bridge module created a `TextEncoder` at load; `AudioWorkletGlobalScope` has none | created on use, in the main-thread control client only |
| The page rejects the emulator's memory as not shared | `instanceof SharedArrayBuffer` fails across the iframe's realm | checks `Object.prototype.toString` instead |
| Dexed underruns 14 times in its first 1.75 s live | first-time JIT translation of paths the 1 s warm-up chord did not reach | warm-up of 8-note chords across the keyboard (3 s, `Vst2Instance::warmUp`) |
| Dexed takes a 15-50 ms turn the first time a buzz-remote song reuses voices whose release had fully decayed (3 underrun blocks at L = 2,048 in one of two runs) | first-time JIT translation the legato warm-up never reached | warm-up of 7 s: plus detached chords with silence between them, transport playing |
| The emulator dies when Dexed is unloaded a third time (`KMemory::commitPreparedCodeInvalidation nextOp->blockStart`) | a Boxedwine MT JIT bug invalidating unmapped code | `Vst2Instance` keeps the module loaded; later instances reuse it and its translated code |
| A Boxedwine panic in a pthread shows only `ReferenceError: alert is not defined` | SDL's message box calls `alert()`, which workers lack, so the panic text is lost | diagnosed by rewriting that call to `console.error` in a local build; `WinVstHost`'s watchdog now silences and reports the machines |
