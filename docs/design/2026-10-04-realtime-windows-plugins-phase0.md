# Phase 0 plan: real-time bridge spike (go/no-go)

Spec: [2026-10-04-realtime-windows-plugins-design.md](2026-10-04-realtime-windows-plugins-design.md)

Goal: prove or disprove, with numbers, that Boxedwine's multithreaded build can
stream a 32-bit Windows plugin (Dexed) into a browser AudioWorklet in real time
through a shared-memory device. Everything stays in `experiments/boxedwine-vst` at the repository root.

## Tasks

1. **MT build baseline.** Build Boxedwine's `multiThreadedJit` target with the
   two existing patches. Run the offline harness against it (`vsthost --serve`,
   PoC Synth VST2/VST3, Dexed). Record boot time, Dexed start-up and the
   realtime factor next to the ST numbers.
   *Gate: Dexed renders correctly in the MT build.*

2. **Device reachability through Wine.** A tiny Windows test program opens
   `Z:\dev\null` and `Z:\dev\urandom` with `CreateFileA`, reads and writes, and
   calls `DeviceIoControl`. Confirms that Wine passes a Boxedwine character
   device through to `read`/`write`/`ioctl`.
   *Fallback if not: the custom-syscall path.*

3. **`/dev/vstbridge` (patch 0003).**
   - Region allocation at start-up and the exported `_vstbridge_region()`.
   - Shared layout header (`vstbridge_abi.h`) and its TypeScript twin, with a
     layout test. Port counts and one ring per stereo port from the start
     (spec §3.7), so engine v2's sidechains don't change the layout later;
     Phase 0 uses one stereo output.
   - Blocking `read` on `requestSeq` with `emscripten_futex_wait`; `write` to
     audio-out; `ATTACH`.
   - A native unit test in Boxedwine's test runner for wrap-around and sequence
     handling.

4. **`vsthost --bridge`.** Control channel commands (`LOAD`, `DESCRIBE`,
   `PING`), one guest thread per instance with the request/response loop,
   FTZ/DAZ, thread priority. Reuse the VST2/VST3 code from the offline host.

5. **Browser test page** (`web/realtime.html`):
   - boots the MT build in a hidden iframe and posts the region to an
     AudioWorklet;
   - the worklet sends one request per block, reads output `L` frames behind and
     counts underruns;
   - an on-screen keyboard plus Web MIDI for live notes;
   - a live readout of underruns, block-time percentiles and the realtime factor.

6. **Measurements** (headless Chromium plus one real laptop browser):
   - wake-up latency (worklet notify → guest `ReadFile` returns);
   - per-block overhead with the PoC Synth;
   - Dexed underruns over 10 minutes of sequenced 8-voice chords at
     `L` = 512 / 1,024 / 2,048 frames;
   - sample-identity of real-time output (shifted by `L`) against the offline render.

7. **Write-up:** `experiments/boxedwine-vst/docs/results.md` gets a real-time section; the spec's open
   questions are answered or carried forward; go/no-go recommendation for
   Phase 1.

## Exit criteria

- **Go:** Dexed streams with zero underruns at `L` ≤ 2,048 frames (≈ 43 ms at
  48 kHz) for 10 minutes, and the real-time output matches the offline render.
- **No-go triggers:** the MT build can't run Wine/JUCE reliably; wake-up or
  scheduling jitter regularly exceeds a block; or Dexed's realtime factor in MT
  is below ~1.5×. Then the fallback is freeze/offline rendering for emulated
  plugins, and the effort moves to option 3 (static recompilation).
