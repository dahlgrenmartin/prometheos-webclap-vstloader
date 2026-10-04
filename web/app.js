// Boots Boxedwine once, with vsthost.exe in its persistent "--serve" mode (the
// counterpart of yabridge's long-lived Wine-side host), then posts one job per
// render into vsthost's mailbox file through the emulator's filesystem and
// polls for the report and WAV that vsthost writes back.
"use strict";

const APP_ZIP = "vstpoc.zip";
const ROOT_ZIP = "TinyCore15Wine11.0";
// The shell keeps an app's Wine prefix at /root/app/<zip name> in Emscripten's FS.
const DRIVE_C = "/root/app/" + encodeURIComponent(APP_ZIP) + "/home/username/.wine/drive_c";
const JOBS_DIR = DRIVE_C + "/vstpoc-jobs";
const OUT_DIR = DRIVE_C + "/vstpoc-out";
const BOOT_TIMEOUT_MS = 15 * 60 * 1000;
const RENDER_TIMEOUT_MS = 10 * 60 * 1000;

const $ = (id) => document.getElementById(id);
let audioBuffer = null;
let audioCtx = null;
let runId = 0;
let emulator = null; // { frame, ready: Promise<Window> }

function setStatus(text, state) {
  $("status").textContent = text;
  $("dot").className = "dot" + (state ? " " + state : "");
}

async function loadPlugins() {
  const list = await (await fetch("plugins.json")).json();
  for (const p of list) {
    const option = document.createElement("option");
    option.value = p.file;
    option.textContent = `${p.label} — ${p.format.toUpperCase()}${p.note ? " · " + p.note : ""}`;
    $("plugin").appendChild(option);
  }
}

function guestExists(win, path) {
  try {
    return Boolean(win && win.FS && win.FS.analyzePath(path).exists);
  } catch (e) {
    return false;
  }
}

function readGuestFile(win, path) {
  try {
    return guestExists(win, path) ? win.FS.readFile(path) : null;
  } catch (e) {
    return null;
  }
}

function waitFor(test, timeoutMs, onTick) {
  const started = performance.now();
  return new Promise((resolve, reject) => {
    const timer = setInterval(() => {
      const elapsed = performance.now() - started;
      if (onTick) onTick(elapsed / 1000);
      let value = null;
      try {
        value = test();
      } catch (e) {
        value = null;
      }
      if (value) {
        clearInterval(timer);
        resolve(value);
      } else if (elapsed > timeoutMs) {
        clearInterval(timer);
        reject(new Error("timed out"));
      }
    }, 500);
  });
}

function startEmulator() {
  const params = new URLSearchParams({
    // Boxedwine resolves files from the first zip that has them, and the shell
    // passes root= first: vstpoc-prefix (see build.sh) shadows the Wine
    // filesystem's .update-timestamp so Wine skips its prefix update.
    root: "vstpoc-prefix",
    overlay: ROOT_ZIP,
    app: APP_ZIP,
    p: "vsthost.exe",
    args: "--serve c:\\vstpoc-jobs",
    storage: "memory",
    sound: "false",
  });
  const frame = document.createElement("iframe");
  frame.title = "Boxedwine emulator";
  frame.src = "boxedwine/boxedwine.html?" + params.toString();
  $("frame-host").innerHTML = "";
  $("frame-host").appendChild(frame);
  const booted = performance.now();
  const ready = waitFor(
    () => guestExists(frame.contentWindow, JOBS_DIR + "/ready") && frame.contentWindow,
    BOOT_TIMEOUT_MS,
    (s) => setStatus(`Booting Wine in the emulator… ${s.toFixed(0)} s (first boot takes a few minutes)`, "busy"),
  ).then((win) => {
    window.vstPocBootSeconds = (performance.now() - booted) / 1000;
    return win;
  });
  emulator = { frame, ready };
  ready.catch(() => {
    emulator = null;
  });
  return emulator;
}

async function render() {
  const plugin = $("plugin").value;
  const notes = $("notes").value.replace(/[^0-9,]/g, "") || "60";
  const seconds = Math.min(10, Math.max(0.5, Number($("seconds").value) || 2));
  const tag = "render" + ++runId;
  $("render").disabled = true;
  $("result").hidden = true;
  try {
    const win = await (emulator || startEmulator()).ready;
    // vsthost's mailbox: write into the guest-created inbox once it is empty
    // (Boxedwine does not see files the page creates, only contents it changes).
    const inbox = `${JOBS_DIR}/inbox.txt`;
    await waitFor(() => {
      const bytes = readGuestFile(win, inbox);
      return bytes && bytes.length === 0;
    }, RENDER_TIMEOUT_MS, (s) => setStatus(`Waiting for the previous job… ${s.toFixed(0)} s`, "busy"));
    const job = [plugin, "--out", `c:\\vstpoc-out\\${tag}.wav`, "--report", `c:\\vstpoc-out\\${tag}.json`,
      "--notes", notes, "--seconds", String(seconds), "end"].join("\n") + "\n";
    win.FS.writeFile(inbox, job);
    const started = performance.now();
    const report = await waitFor(() => {
      const bytes = readGuestFile(win, `${OUT_DIR}/${tag}.json`);
      return bytes && bytes.length > 0 ? JSON.parse(new TextDecoder().decode(bytes)) : null;
    }, RENDER_TIMEOUT_MS, (s) => setStatus(`vsthost.exe is rendering ${plugin} under Wine… ${s.toFixed(0)} s`, "busy"));
    const wall = (performance.now() - started) / 1000;
    if (!report.ok) throw new Error(report.error || "vsthost reported a failure");
    const wav = await waitFor(() => {
      const bytes = readGuestFile(win, `${OUT_DIR}/${tag}.wav`);
      return bytes && bytes.length >= 44 + report.frames * 4 ? bytes : null;
    }, 30000);
    await showResult(report, wav, wall);
    setStatus(`Rendered ${report.name} in the browser: ${wall.toFixed(1)} s for this job` +
      (window.vstPocBootSeconds ? ` (emulator boot ${window.vstPocBootSeconds.toFixed(0)} s, once).` : "."), "ok");
    window.vstPocResult = { report, wavBytes: wav.length, wallSeconds: wall, bootSeconds: window.vstPocBootSeconds };
  } catch (error) {
    setStatus("Failed: " + error.message, "bad");
    window.vstPocResult = { error: error.message };
  } finally {
    $("render").disabled = false;
  }
}

async function showResult(report, wav, wall) {
  audioCtx = audioCtx || new (window.AudioContext || window.webkitAudioContext)();
  audioBuffer = await audioCtx.decodeAudioData(wav.slice().buffer);
  drawWave(audioBuffer);
  const url = URL.createObjectURL(new Blob([wav], { type: "audio/wav" }));
  $("download").href = url;
  $("download").download = `${report.name || "render"}.wav`;
  const facts = [
    ["Plugin", report.name],
    ["Vendor", report.vendor],
    ["Format", report.format.toUpperCase() + (report.synth ? " instrument" : " effect")],
    ["Binary", report.plugin],
    ["Audio", `${report.frames} frames @ ${report.sampleRate} Hz, peak ${report.peak.toFixed(3)}, RMS ${report.rms.toFixed(3)}`],
    ["Plugin start-up", `${report.loadMs.toFixed(0)} ms load + ${report.initMs.toFixed(0)} ms init (one-off per load)`],
    ["Audio processing", `${report.processMs.toFixed(0)} ms (${report.realtimeFactor.toFixed(2)}× realtime, emulated)`],
    ["Job round trip", `${wall.toFixed(1)} s`],
  ];
  $("facts").innerHTML = "";
  for (const [k, v] of facts) {
    const tr = $("facts").insertRow();
    const th = document.createElement("th");
    th.textContent = k;
    tr.appendChild(th);
    tr.insertCell().textContent = v;
  }
  $("params").innerHTML = "<tr><th>Parameter</th><th>Value</th></tr>";
  for (const p of report.params.slice(0, 24)) {
    const tr = $("params").insertRow();
    tr.insertCell().textContent = p.name;
    tr.insertCell().textContent = p.display || p.value.toFixed(3);
  }
  if (report.params.length > 24) {
    $("params").insertRow().insertCell().textContent = `… ${report.params.length - 24} more`;
  }
  $("result").hidden = false;
}

function drawWave(buffer) {
  const canvas = $("wave");
  const ctx = canvas.getContext("2d");
  const data = buffer.getChannelData(0);
  const w = canvas.width, h = canvas.height;
  ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = getComputedStyle(document.documentElement).getPropertyValue("--wave").trim() || "#2f7fe0";
  ctx.beginPath();
  const step = Math.max(1, Math.floor(data.length / w));
  for (let x = 0; x < w; x++) {
    let lo = 1, hi = -1;
    for (let i = x * step; i < (x + 1) * step && i < data.length; i++) {
      lo = Math.min(lo, data[i]);
      hi = Math.max(hi, data[i]);
    }
    ctx.moveTo(x + 0.5, (1 - hi) * h / 2);
    ctx.lineTo(x + 0.5, (1 - lo) * h / 2);
  }
  ctx.stroke();
}

$("render").addEventListener("click", render);
$("play").addEventListener("click", () => {
  if (!audioBuffer) return;
  audioCtx.resume();
  const src = audioCtx.createBufferSource();
  src.buffer = audioBuffer;
  src.connect(audioCtx.destination);
  src.start();
});
loadPlugins().then(() => {
  const wanted = new URLSearchParams(location.search).get("plugin");
  if (wanted) $("plugin").value = wanted;
  if (new URLSearchParams(location.search).has("autorun")) render();
});
