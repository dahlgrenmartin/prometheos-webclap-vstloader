// Drives web/realtime.html in headless Chromium:
//   node tests/realtime.mjs <base url> [--plugin Dexed.dll] [--latency 1024] [--block 256]
//        [--seconds 600] [--capture 15] [--bench 2000] [--out dir] [--shot file.png]
// Prints the boot, plugin start-up and stream statistics as JSON lines; with
// --capture and --out, writes <out>/capture.bin (vsthost --replay input: a
// "VSRP" header and the published requests) and <out>/realtime.f32 (the played
// output, interleaved stereo float32) for tests/identity.mjs.
// CHROMIUM=/path/to/chrome uses a preinstalled browser.
import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { chromium } from "playwright";

const [base, ...rest] = process.argv.slice(2);
const opt = { plugin: "Dexed.dll", latency: "1024", block: "256", seconds: "30", capture: "0", bench: "0", out: "", shot: "" };
for (let i = 0; i < rest.length; i += 2) opt[rest[i].replace(/^--/, "")] = rest[i + 1];

const browser = await chromium.launch({
  executablePath: process.env.CHROMIUM || undefined,
  args: ["--autoplay-policy=no-user-gesture-required"],
});
const page = await browser.newPage({ viewport: { width: 1100, height: 1000 } });
page.on("pageerror", (e) => console.log(JSON.stringify({ pageerror: e.message })));
page.on("console", (m) => {
  const t = m.text();
  if (/vsthost:|err:|error/i.test(t) && !/callHandler/.test(t)) console.log(JSON.stringify({ console: t.replace(/\x1b\[1C/g, " ").replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "") }));
});
const query = new URLSearchParams({ plugin: opt.plugin, latency: opt.latency, block: opt.block, autostart: "1",
  sequence: "1", seconds: opt.seconds, capture: opt.capture, bench: opt.bench });
await page.goto(`${base}/realtime.html?${query}`);

const started = Date.now();
let lastPrint = 0;
let r;
for (;;) {
  await new Promise((resolve) => setTimeout(resolve, 2000));
  r = await page.evaluate(() => {
    const v = window.vstRealtime;
    return { phase: v.phase, status: v.status, error: v.error, bootSeconds: v.bootSeconds, loadMs: v.loadMs,
      benchLoadMs: v.benchLoadMs, bench: v.bench, stats: v.stats, ping: v.ping, audioContextState: v.audioContextState,
      describe: v.describe && { name: v.describe.name, params: v.describe.params.length, inPorts: v.describe.inPorts,
        outPorts: v.describe.outPorts, latency: v.describe.latency },
      capture: v.capture && { blocks: v.capture.blocks, frames: v.capture.frames, recordBytes: v.capture.recordBytes } };
  });
  if (r.phase === "failed") break;
  if (Date.now() - lastPrint > 30000 && r.stats) {
    lastPrint = Date.now();
    const s = r.stats;
    console.log(JSON.stringify({ t: (Date.now() - started) / 1000, streamed: s.seconds, underrunBlocks: s.underrunBlocks,
      minMargin: s.minMarginFrames, processUs: s.processUs, skipped: s.skipped }));
  }
  const captureReady = !Number(opt.capture) || r.capture;
  if (r.phase === "done" && captureReady) break;
  if (Date.now() - started > (Number(opt.seconds) + 1200) * 1000) {
    r.error = "driver timed out";
    break;
  }
}
console.log(JSON.stringify({ result: { ...r, wall: (Date.now() - started) / 1000 } }));
if (opt.shot) await page.screenshot({ path: opt.shot, fullPage: true });

if (opt.out && r.capture) {
  mkdirSync(opt.out, { recursive: true });
  const fetchAll = async (which) => {
    const total = await page.evaluate((w) => window.vstRealtime.capture[w].byteLength, which);
    const parts = [];
    for (let at = 0; at < total; at += 4 << 20) {
      parts.push(Buffer.from(await page.evaluate(([w, o]) => window.vstRealtimeCaptureChunk(w, o, 4 << 20), [which, at]), "base64"));
    }
    return Buffer.concat(parts);
  };
  const requests = await fetchAll("requests");
  const output = await fetchAll("output");
  const header = Buffer.alloc(32);
  header.write("VSRP", 0, "ascii");
  header.writeUInt32LE(1, 4);
  header.writeUInt32LE(48000, 8);
  header.writeUInt32LE(Number(opt.block), 12);
  header.writeUInt32LE(r.describe.inPorts.length, 16);
  header.writeUInt32LE(r.describe.outPorts.length, 20);
  header.writeUInt32LE(r.capture.blocks, 24);
  header.writeUInt32LE(1, 28); // warm-up, as the bridge does
  writeFileSync(join(opt.out, "capture.bin"), Buffer.concat([header, requests]));
  writeFileSync(join(opt.out, "realtime.f32"), output);
  writeFileSync(join(opt.out, "realtime.json"), JSON.stringify({ ...opt, ...r }, null, 2));
  console.log(JSON.stringify({ wrote: opt.out, requestBytes: requests.length, outputBytes: output.length }));
}
await browser.close();
const ok = r.phase === "done" && !r.error && r.stats && r.stats.underrunBlocks === 0;
process.exit(ok ? 0 : 1);
