# Performance: where the time goes, and how to make it real-time

## Why this is not yabridge-fast

yabridge runs a Windows plugin's x86-64 machine code **directly on an x86-64
CPU**. Wine is not an emulator: it reimplements the Windows API, so the
plugin's DSP runs at full native speed. yabridge's latency cost is only the IPC
between the Linux plugin shim and the Wine host (sockets and shared memory).

In the browser there is no x86 CPU. Boxedwine has to **translate every x86
instruction** to WebAssembly at run time, and then the browser compiles that
WebAssembly to the real CPU's instructions. "WebAssembly is about 1.5× slower
than native" holds for code *compiled from source* to WebAssembly. It does not
hold for an x86 emulator running inside WebAssembly.

The same PoC Synth DSP code, rendering the same 2-second chord, at each layer:

| How the code runs | Time for 2 s of audio | vs. native |
|---|---|---|
| Native x86-64, `gcc -O2` | 0.86 ms | 1× |
| WebAssembly compiled from source (`emcc -O2`, Node) | 0.79 ms | ~1× |
| x86 DLL under Boxedwine, native x64 JIT | ~40 ms | ~45× slower |
| x86 DLL under Boxedwine, **WebAssembly JIT in the browser** | 97–170 ms | **~110–200× slower** |

Boxedwine's own documentation explains the structure. It translates groups of
about 5 x86 instructions (each group ends at a branch), chains them with
indirect calls, keeps guest registers and flags in memory, and calls helper
functions for much of the x87/SSE floating point. A block-at-a-time translator
pays that per-block cost on every iteration of a DSP loop. Leaning
Technologies' CheerpX, a commercial x86-to-WebAssembly engine with a tiered,
optimizing JIT, reports 5–10× slower than native for complex programs and 2–3×
at best. It is 32-bit only and does not run Wine yet.

## What we measured for real plugins (browser, single-threaded `jit` build)

`vsthost` now reports plugin start-up (`initMs`) separately from the audio loop
(`processMs`); `realtimeFactor` is audio length ÷ `processMs`.

| Plugin | First render | Later renders | Audio processing speed |
|---|---|---|---|
| PoC Synth VST2 | init 0.2 s | — | **12–20× realtime** |
| PoC Synth VST3 | init 0 s | — | **~12× realtime** |
| Dexed 0.9.3 (JUCE, MSVC) | `VSTPluginMain` **9.1 s** (one-off JUCE/Wine GUI start-up and JIT warm-up) | init 0.55 s | **3.8–4.7× realtime, from the first render** |

The earlier "0.2× realtime" for Dexed was almost entirely the first
`VSTPluginMain` and per-job `LoadLibrary`/`FreeLibrary` (unloading Dexed takes
6–8 s while JUCE shuts its threads down). A real-time host loads the plugin
once and keeps it, so neither cost recurs. **At about 4× headroom, the
current single-threaded emulator can already run Dexed in real time.**

A negative result: Boxedwine's recorded WASM JIT cache (`jit-record`, 22,519
blocks, 28 MB) did *not* help this workload. Importing it added about 11 s to
start-up, Dexed's first `VSTPluginMain` stayed about 11 s, and recording mode's
relocatable code made the steady state about 30 % slower.

## Options, from "works now" to "big bet"

### 1. Real-time streaming with the plugin kept loaded (engineering, low risk)

This is what yabridge does, with the emulator in place of the native CPU:

- `vsthost` loads the plugin once and runs a block loop (128–512 frames).
- Audio and MIDI cross a **shared-memory ring buffer** instead of files. This
  needs one small native addition to Boxedwine: a guest-visible device or
  syscall backed by a JavaScript `SharedArrayBuffer`, yabridge's shared memory
  in emulator form. Boxedwine already moves guest audio to an AudioWorklet
  through a `SharedArrayBuffer` ring with a 30 ms target.
- Run the emulator off the main thread (Boxedwine's `multiThreadedJit` target,
  which its docs put at about 50 % faster).
- **Render ahead for sequenced material.** In a tracker or DAW (buzz-remote),
  pattern notes are known ahead of time. They can be sent with sample
  timestamps and rendered early, so playback latency is effectively zero. Only
  live input and knob moves see the ring-buffer latency (about 10–30 ms, in
  yabridge's range).
- Budget: Dexed at about 4× headroom leaves room for polyphony spikes. Heavier
  plugins will not fit.

### 2. Faster translation of plugin code (emulator work, medium risk)

- **Region/function-level JIT** for hot plugin code: translate whole loops or
  functions with guest registers in WebAssembly locals and dead flag
  computations removed, instead of 5-instruction blocks behind indirect calls.
  That is the difference between Boxedwine's ~100–200× and CheerpX's 2–10×.
  It would be a contribution to Boxedwine's WebAssembly JIT and would help
  every guest program.
- Map more SSE/x87 work straight to WebAssembly SIMD and float ops instead of
  helper calls.

### 3. "Plugin recomp": static recompilation of the DLL (research, high payoff)

Treat the plugin DLL the way [N64Recomp](https://github.com/N64Recomp/N64Recomp)
treats a game ROM: translate it **ahead of time**, and drop Wine and the
emulator from the audio path.

- Lift the PE's x86 or **x86-64** code to LLVM IR with
  [remill](https://github.com/lifting-bits/remill), which covers integer, x87,
  SSE and AVX, then optimize and compile to WebAssembly with Emscripten.
  [elfconv](https://fosdem.org/2024/schedule/event/fosdem-2024-2254-elfconv-aot-compiler-that-translates-linux-aarch64-elf-binary-to-llvm-bitcode-targeting-webassembly/)
  already does remill → LLVM → Emscripten for AArch64 Linux ELF files.
- Load it with a **minimal PE loader and Win32 shim** instead of Wine:
  relocations, imports, heap, TLS, critical sections, `msvcrt` math, and stubs
  for the `user32`/`gdi32` calls JUCE makes at start-up. Tavis Ormandy's
  [loadlibrary](https://github.com/taviso/loadlibrary) and decompals'
  [wibo](https://github.com/decompals/wibo) show that self-contained Windows
  DLLs and tools run with a small loader.
- Package the result as a **WebCLAP** module, so buzz-remote's existing WebCLAP
  host can run it like the OB-Xd port.
- Expected speed: lifted code is typically a few times slower than native
  before optimization, so tens of times faster than the emulator. It also
  covers **64-bit plugins**, which Boxedwine (and CheerpX) cannot run.
- Risks: indirect jumps and vtables (the relocation table helps, and a
  runtime fallback can catch misses), SEH and C++ exceptions, plugin threads,
  copy protection (out of scope), and editors (start with generic parameter
  UIs). Recompiling a commercial binary also raises licensing questions;
  freeware and user-owned local use are the safe first targets.

### 4. Source available: port to WebCLAP (best quality, already proven)

For open-source plugins, compiling from source gives native-equivalent speed
(the table above: 0.79 ms vs 0.86 ms), as the OB-Xd WebCLAP port does. Dexed
(GPL-3) is a good next candidate.

### 5. Leave the sandbox: a local companion

A native helper (yabridge, or a small CLAP/VST host) on the user's machine,
reached over WebSocket, WebTransport, WebRTC or Chrome native messaging. This
gives full native speed, 64-bit plugins and native editor windows, with a few
milliseconds of latency on localhost. It is not "in the browser", but it is
how to support arbitrary commercial plugins today.

## Recommendation

1. **Now:** option 1, real-time streaming in buzz-remote with render-ahead,
   for 32-bit plugins that fit the budget (Dexed does).
2. **Every open-source plugin:** option 4.
3. **The big bet for closed-source and 64-bit plugins:** option 3. Start with
   a spike that recompiles the PoC Synth DLL, then a simple freeware 32-bit
   effect, then a JUCE plugin.

Sources: [CheerpX 1.0](https://labs.leaningtech.com/blog/cx-10),
[CheerpX](https://labs.leaningtech.com/cheerpx),
[remill](https://trailofbits.com/tools/remill/),
[McSema](https://github.com/SRI-CSL/mcsema),
[elfconv (FOSDEM 2024)](https://fosdem.org/2024/schedule/event/fosdem-2024-2254-elfconv-aot-compiler-that-translates-linux-aarch64-elf-binary-to-llvm-bitcode-targeting-webassembly/),
[N64Recomp](https://github.com/N64Recomp/N64Recomp),
[loadlibrary](https://www.phoronix.com/news/LoadLibrary-DLLs-On-Linux),
[wibo](https://git.axiodl.com/decompals/wibo/raw/tag/0.2.0/README.md),
Boxedwine `docs/CPUemulation.md` and `docs/Performance.md`.
