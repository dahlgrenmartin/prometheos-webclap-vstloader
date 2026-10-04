// Real-time test page (Phase 0): boots Boxedwine's multithreaded build in a
// hidden iframe running vsthost --bridge, loads a plugin on a vstbridge
// channel and streams it through an AudioWorklet. URL parameters run it
// without a user (tests/realtime.mjs):
//   plugin=Dexed.dll latency=1024 block=256 autostart=1 sequence=1
//   seconds=600 (stop after) capture=15 (seconds kept for the identity test)
//   bench=2000 (wake-up/overhead benchmark on channel 2 with the PoC Synth first)
import { BridgeRegion, ControlClient, VSTB } from "./vstbridge.js";

const E = VSTB.enums;
const params = new URLSearchParams(location.search);
const $ = (id) => document.getElementById(id);
const BOOT_TIMEOUT_MS = 15 * 60 * 1000;

const result = (window.vstRealtime = { phase: "idle" });

function setStatus(text, state) {
  $("status").textContent = text;
  $("dot").className = "dot" + (state ? " " + state : "");
  result.status = text;
}

async function loadPluginList() {
  const list = await (await fetch("plugins.json")).json();
  for (const p of list.filter((p) => p.format === "vst2")) {
    const option = document.createElement("option");
    option.value = p.file;
    option.textContent = `${p.label}${p.note ? " · " + p.note : ""}`;
    $("plugin").appendChild(option);
  }
  for (const id of ["plugin", "latency", "block"]) if (params.get(id)) $(id).value = params.get(id);
}

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function bootEmulator() {
  const query = new URLSearchParams({
    root: "vstpoc-prefix",
    overlay: "TinyCore15Wine11.0",
    app: "vstpoc.zip",
    p: "vsthost.exe",
    args: "--bridge",
    storage: "memory",
    sound: "false",
  });
  const frame = document.createElement("iframe");
  frame.title = "Boxedwine emulator";
  frame.src = "boxedwine/boxedwine.html?" + query;
  $("frame-host").appendChild(frame);
  const started = performance.now();
  for (;;) {
    const elapsed = performance.now() - started;
    if (elapsed > BOOT_TIMEOUT_MS) throw new Error("emulator boot timed out");
    setStatus(`Booting Wine in the emulator… ${(elapsed / 1000).toFixed(0)} s`, "busy");
    const mod = frame.contentWindow && frame.contentWindow.Module;
    if (mod && typeof mod._vstbridge_region === "function" && mod.HEAPU8) {
      const buffer = mod.HEAPU8.buffer;
      // (instanceof fails across the iframe's realm)
      if (Object.prototype.toString.call(buffer) !== "[object SharedArrayBuffer]") throw new Error("emulator memory is not shared (multithreaded build and COOP/COEP needed)");
      const region = new BridgeRegion(buffer, mod._vstbridge_region());
      if (region.valid && region.serving) {
        result.bootSeconds = (performance.now() - started) / 1000;
        return { frame, region, control: new ControlClient(region) };
      }
    }
    await sleep(250);
  }
}

async function load(emulator, channel, plugin, rate, block) {
  const t0 = performance.now();
  const reply = await emulator.control.request(E.OP_LOAD, channel,
    `rate=${rate} block=${block} warmup=1 path=C:\\files\\${plugin}`);
  if (reply.status !== E.STATUS_OK) throw new Error(`LOAD ${plugin}: ${reply.text}`);
  return { describe: JSON.parse(reply.text), loadMs: performance.now() - t0 };
}

function runBench(emulator, channel, block, iterations) {
  return new Promise((resolve, reject) => {
    const worker = new Worker("bench-worker.js", { type: "module" });
    worker.onmessage = (e) => (e.data.error ? reject(new Error(e.data.error)) : resolve(e.data), worker.terminate());
    worker.onerror = (e) => reject(new Error(e.message));
    worker.postMessage({ buffer: emulator.region.buffer, base: emulator.region.base, channel, block, iterations, warmup: 200 });
  });
}

function showStats(s, extra) {
  const items = [
    ["Streamed", `${s.seconds.toFixed(1)} s`],
    ["Underrun blocks", s.underrunBlocks],
    ["Min margin", `${Number.isFinite(s.minMarginFrames) ? s.minMarginFrames : "–"} frames`],
    ["Block time p50 / p99", s.processUs ? `${(s.processUs.p50 / 1000).toFixed(2)} / ${(s.processUs.p99 / 1000).toFixed(2)} ms` : "–"],
    ["Block time max", s.processUs ? `${(s.processUs.max / 1000).toFixed(2)} ms` : "–"],
    ["Realtime factor (p50)", s.processUs ? `${(s.blockUs / s.processUs.p50).toFixed(2)}×` : "–"],
    ["Block budget", `${(s.blockUs / 1000).toFixed(2)} ms`],
    ...extra,
  ];
  $("stats").innerHTML = "";
  for (const [k, v] of items) {
    const div = document.createElement("div");
    div.className = "stat";
    div.innerHTML = `<div class="v"></div><div class="k"></div>`;
    div.firstChild.textContent = String(v);
    div.lastChild.textContent = k;
    $("stats").appendChild(div);
  }
}

let node = null;

function buildKeyboard() {
  const keys = $("keys");
  const whiteOrder = [0, 2, 4, 5, 7, 9, 11];
  const computer = "awsedftgyhujk";
  const down = new Set();
  const send = (note, on) => {
    if (!node) return;
    if (on === down.has(note)) return;
    on ? down.add(note) : down.delete(note);
    node.port.postMessage({ type: "note", note, on, velocity: 100 });
    keys.querySelector(`[data-note="${note}"]`)?.classList.toggle("down", on);
  };
  let white = 0;
  for (let note = 48; note <= 84; note++) {
    const key = document.createElement("div");
    const isWhite = whiteOrder.includes(note % 12);
    key.className = "key" + (isWhite ? "" : " black");
    key.dataset.note = String(note);
    if (isWhite) {
      key.textContent = note % 12 === 0 ? `C${note / 12 - 1}` : "";
      white++;
    } else {
      key.style.left = `${white * 34 - 11}px`;
    }
    key.addEventListener("pointerdown", (e) => (key.setPointerCapture(e.pointerId), send(note, true)));
    key.addEventListener("pointerup", () => send(note, false));
    key.addEventListener("pointercancel", () => send(note, false));
    keys.appendChild(key);
  }
  addEventListener("keydown", (e) => {
    const i = computer.indexOf(e.key);
    if (i >= 0 && !e.repeat) send(60 + i, true);
  });
  addEventListener("keyup", (e) => {
    const i = computer.indexOf(e.key);
    if (i >= 0) send(60 + i, false);
  });
  navigator.requestMIDIAccess?.().then((midi) => {
    for (const input of midi.inputs.values()) {
      input.onmidimessage = ({ data }) => {
        const [status, note, velocity] = data;
        if ((status & 0xf0) === 0x90) send(note, velocity > 0);
        else if ((status & 0xf0) === 0x80) send(note, false);
      };
    }
  }).catch(() => {});
}

async function start() {
  $("start").disabled = true;
  const plugin = $("plugin").value;
  const L = Number($("latency").value);
  const B = Number($("block").value);
  const rate = Number(params.get("rate") || 48000);
  const seconds = Number(params.get("seconds") || 0);
  Object.assign(result, { plugin, latency: L, block: B, rate, phase: "booting" });
  try {
    // The audio context comes first: browsers only allow it in the click.
    const ctx = new AudioContext({ sampleRate: rate, latencyHint: "interactive" });
    await ctx.audioWorklet.addModule("realtime-worklet.js");
    const emulator = await bootEmulator();
    result.phase = "loading";
    setStatus(`Emulator up after ${result.bootSeconds.toFixed(1)} s; loading ${plugin}…`, "busy");
    const pong = await emulator.control.request(E.OP_PING);
    result.ping = pong.text;
    const loaded = await load(emulator, 1, plugin, rate, B);
    Object.assign(result, loaded);
    const benchIterations = Number(params.get("bench") || 0);
    if (benchIterations) {
      result.phase = "bench";
      setStatus(`Benchmarking wake-up with the PoC Synth (${benchIterations} blocks)…`, "busy");
      const bench = await load(emulator, 2, "PoCSynth.dll", rate, B);
      result.benchLoadMs = bench.loadMs;
      result.bench = await runBench(emulator, 2, B, benchIterations);
    }
    const describe = loaded.describe;
    const ch = emulator.region.channel(1);
    node = new AudioWorkletNode(ctx, "vstbridge-stream", {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [2],
      processorOptions: {
        buffer: emulator.region.buffer, base: emulator.region.base, channel: 1, block: B, latency: L,
        inPorts: describe.inPorts.length, outPorts: describe.outPorts.length, startBlock: ch.word("requestSeq"),
        sequence: params.get("sequence") === "1", captureSeconds: Number(params.get("capture") || 0),
      },
    });
    node.connect(ctx.destination);
    node.port.onmessage = (e) => {
      const m = e.data;
      if (m.type === "stats") {
        result.stats = m;
        showStats(m, [["Plugin", describe.name], ["Plugin start-up", `${(loaded.loadMs / 1000).toFixed(1)} s`],
          ["Boot", `${result.bootSeconds.toFixed(1)} s`], ["Latency L", `${L} frames (${((L / rate) * 1000).toFixed(1)} ms)`]]);
        if (seconds && m.seconds >= seconds && result.phase === "streaming") {
          result.phase = "done";
          node.port.postMessage({ type: "stop" });
          setStatus(`Done: ${m.seconds.toFixed(0)} s streamed, ${m.underrunBlocks} underrun blocks.`, m.underrunBlocks ? "bad" : "ok");
        }
      } else if (m.type === "capture") {
        result.capture = { blocks: m.blocks, recordBytes: m.recordBytes, frames: m.frames,
          requests: new Uint8Array(m.requests), output: new Float32Array(m.output) };
      }
    };
    await ctx.resume();
    result.phase = "streaming";
    result.audioContextState = ctx.state;
    setStatus(`Streaming ${describe.name} at ${rate} Hz, L = ${L} frames.`, "ok");
  } catch (error) {
    result.phase = "failed";
    result.error = error.message;
    setStatus("Failed: " + error.message, "bad");
    $("start").disabled = false;
  }
}

// For tests: one capture array as base64, in pieces.
window.vstRealtimeCaptureChunk = (which, offset, length) => {
  const array = result.capture[which];
  const bytes = new Uint8Array(array.buffer, array.byteOffset + offset, Math.min(length, array.byteLength - offset));
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
};

$("start").addEventListener("click", start);
buildKeyboard();
loadPluginList().then(() => {
  if (params.get("autostart") === "1") start();
});
