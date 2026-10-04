# Windows VST2 machine MVP: session brief

The detailed scope, done criteria and known traps for the session that builds
the first real-time Windows VST2 machine in buzz-remote. The design is
[the real-time Windows plugins spec](2026-10-04-realtime-windows-plugins-design.md);
Phase 0 is [its plan](2026-10-04-realtime-windows-plugins-phase0.md).

Goal: a 32-bit Windows VST2 plugin binary (`.dll`) running as a real-time
buzz-remote machine, browser-only, through Wine inside Boxedwine (an x86
emulator compiled to WebAssembly). No backend, no native helper.

## Read first

In this order (they are the plan; follow them unless measurements prove them wrong, and record any deviation in the spec):
1. apps/buzz-remote/docs/superpowers/specs/2026-10-04-realtime-windows-plugins-design.md (the design)
2. apps/buzz-remote/docs/superpowers/plans/2026-10-04-realtime-windows-plugins-phase0.md (Phase 0 tasks and go/no-go)
3. apps/buzz-remote/docs/superpowers/specs/2026-10-04-engine-v2-ports-sidechain-design.md (engine v2: MachineInstance.latency(), ports, delay compensation)
4. experiments/boxedwine-vst/ (working offline PoC: README, docs/results.md, docs/performance.md, host/vsthost.cpp, web/app.js, build.sh, patches/)

## What already works (offline)

vsthost.exe --serve renders VST2/VST3 jobs under Boxedwine's single-threaded `jit` build in headless Chromium. Dexed 0.9.3 (JUCE, 155 params) processes at 3.8-4.7x realtime; boot ~20 s; Dexed's first VSTPluginMain ~9 s, then ~0.5 s.

## Scope (VST2 only; VST3 later)

A. Phase 0 spike, in experiments/boxedwine-vst:
   - Build Boxedwine 509f6a7 `multiThreadedJit` with patches 0001/0002; rerun the offline harness on it (gate: Dexed renders correctly; record boot, start-up and realtime factor next to the ST numbers).
   - Prove Wine reaches a Boxedwine device (`Z:\dev\...` via CreateFile/ReadFile/WriteFile/DeviceIoControl); else fall back to the custom-syscall path from the spec.
   - Patch 0003: `/dev/vstbridge`, region in shared wasm memory allocated at start-up, layout header `vstbridge_abi.h` + TS twin with a layout test, per-port rings, guest blocking read via emscripten_futex_wait woken by the worklet's Atomics.notify (no main-thread hop).
   - `vsthost --bridge`: LOAD/DESCRIBE/PING, one guest thread per instance, request/response loop, FTZ/DAZ.
   - web/realtime.html: AudioWorklet streams Dexed live (on-screen keyboard), shows underruns, block-time percentiles, realtime factor.
   - Apply the plan's go/no-go and write the numbers into docs/results.md. If no-go, stop, write up why, and tell me before Phase 1.
B. Phase 1 MVP in apps/buzz-remote (only after a go):
   - `winvst` plugin format next to webclap/webvst: install a user-supplied .dll, DESCRIBE once, freeze the descriptor (name, vendor, params with display text, ports, latency).
   - WinVstHost (main thread: hidden iframe with the MT build, boot progress, control channel, watchdog) and WinVstMachine (worklet: per-block request, read output L frames behind, underrun telemetry, no allocation in process), reporting `latency()` = bridge L + plugin initialDelay so engine v2 compensates.
   - Parameters as buzz globals (same encoding as the WebCLAP backend), notes from patterns, state via effGetChunk/effSetChunk in a `prometheos.winvst/1` project extension, .bzw round trip.
   - Lazy boot (first winvst machine or project load), warm-up render after load.

## Done when

All verified, with evidence in docs/results.md and screenshots):
- In buzz-remote in headless Chromium, a song with Dexed as a winvst machine plays sequenced 8-voice material at 48 kHz with zero underruns for 10 minutes at the chosen L (≤ 2048 frames), and the output shifted by L is sample-identical to vsthost's offline render of the same events.
- The PoC Synth VST2 passes the same identity test; a parallel dry + PoC invert effect nulls (proves compensation), or that test is written and blocked only on engine v2 with the reason stated.
- Save/reload of the .bzw restores Dexed's state; lint, typecheck and the buzz-remote test suite pass; CI workflow boxedwine-vst.yml still passes.

## Known traps from the PoC

- Emscripten port downloads (zlib, SDL2) may 403 through the proxy: clone madler/zlib@v1.3.2 and libsdl-org/SDL@release-2.32.10 into emsdk's cache/ports/{zlib,sdl2} with a matching .emscripten_url marker. Re-run `./emsdk activate latest` if the toolchain isn't found. Boxedwine sources have mixed CRLF; patch with care.
- Wine reruns wineboot on every fresh in-memory prefix and times out (~300 s) unless vstpoc-prefix.zip (`.update-timestamp` = disable) is layered first via `root=`.
- Boxedwine caches directory listings: files the page creates are invisible to the guest; only rewrite contents of guest-created files.
- Native headless Boxedwine hangs on any window creation, so JUCE plugins (Dexed) only work in the browser build; test them there.
- The x87 FRNDINT chop bug (patch 0001) made exp/pow return 1.0; tests/fputest.c reproduces it. Suspect the FPU first if a plugin renders silence.
- Separate one-off start-up (load/init) from processing time in every measurement.
- Boxedwine's README says its multithreaded build "stutters with sound" (cause undocumented); measure block jitter directly.
- Serve with COOP/COEP (serve.py) or SharedArrayBuffer is unavailable.
- Only 32-bit plugins load. Users supply their own binaries; don't commit third-party plugins (build.sh downloads Dexed with WITH_DEXED=1).

## Working style

Work in small validated commits; report progress at each phase gate with the measured numbers.
