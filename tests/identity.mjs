// Sample-identity check for the real-time bridge: renders a capture from
// tests/realtime.mjs offline with vsthost --replay (a separate emulator
// session, by default the single-threaded offline build), then compares the
// played real-time output, shifted by L, with that render, bit for bit.
//   node tests/identity.mjs <base url of a built site> <capture dir> [--plugin Dexed.dll]
// The site's boxedwine/ directory gets vstpoc-replay.zip (the app zip plus the
// capture). CHROMIUM=/path/to/chrome uses a preinstalled browser.
import { execFileSync } from "node:child_process";
import { copyFileSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const [base, dir, ...rest] = process.argv.slice(2);
const opt = { plugin: "", site: join(root, "dist-st") };
for (let i = 0; i < rest.length; i += 2) opt[rest[i].replace(/^--/, "")] = rest[i + 1];
const run = JSON.parse(readFileSync(join(dir, "realtime.json"), "utf8"));
const plugin = opt.plugin || run.plugin;
const L = Number(run.latency);
const B = Number(run.block);

// The app zip plus the capture, served next to the site's other zips.
const zip = join(opt.site, "boxedwine", "vstpoc-replay.zip");
rmSync(zip, { force: true });
copyFileSync(join(root, "build", "vstpoc.zip"), zip);
copyFileSync(join(dir, "capture.bin"), join(root, "build", "capture.bin"));
execFileSync("zip", ["-q", "-X", "-j", zip, join(root, "build", "capture.bin")]);

const browser = await chromium.launch({ executablePath: process.env.CHROMIUM || undefined });
const page = await browser.newPage();
let report = null;
const done = new Promise((resolve) => {
  page.on("console", (m) => {
    const text = m.text().replace(/\x1b\[1C/g, " ").replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "");
    if (/vsthost: replay (done|failed)/.test(text)) resolve();
  });
});
const params = new URLSearchParams({
  root: "vstpoc-prefix", overlay: "TinyCore15Wine11.0", app: "vstpoc-replay.zip", p: "vsthost.exe",
  args: `--replay C:\\files\\${plugin} C:\\files\\capture.bin C:\\vstpoc-out\\ref.f32`, storage: "memory", sound: "false",
});
const started = Date.now();
await page.goto(`${base}/boxedwine/boxedwine.html?${params}`);
await Promise.race([done, new Promise((_, reject) => setTimeout(() => reject(new Error("replay timed out")), 1800000))]);
const path = "/root/app/vstpoc-replay.zip/home/username/.wine/drive_c/vstpoc-out/ref.f32";
report = JSON.parse(await page.evaluate((p) => new TextDecoder().decode(FS.readFile(p + ".json")), path));
if (!report.ok) throw new Error("replay failed: " + report.error);
const b64 = await page.evaluate((p) => {
  const bytes = FS.readFile(p);
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(s);
}, path);
await browser.close();
const refBytes = Buffer.from(b64, "base64");
writeFileSync(join(dir, "reference.f32"), refBytes);
const ref = new Float32Array(refBytes.buffer, refBytes.byteOffset, refBytes.length / 4);
const rtBytes = readFileSync(join(dir, "realtime.f32"));
const rt = new Float32Array(rtBytes.buffer, rtBytes.byteOffset, rtBytes.length / 4);

// ref: per block, planar [L x B][R x B]; rt: interleaved stereo from stream start.
const blocks = Math.min(report.blocks, Math.floor((rt.length / 2 - L) / B));
let identical = 0, silentInRealtime = 0, mismatched = 0, maxDiff = 0, firstMismatch = -1, peak = 0;
// A block that differs only where the real-time output is silent was (partly)
// played as silence: an underrun, not a wrong sample.
const rtBits = new Uint32Array(rt.buffer, rt.byteOffset, rt.length);
const refBits = new Uint32Array(ref.buffer, ref.byteOffset, ref.length);
for (let k = 0; k < blocks; k++) {
  let same = true, onlyWhereSilent = true;
  for (let c = 0; c < 2; c++) {
    for (let i = 0; i < B; i++) {
      const r = k * 2 * B + c * B + i;
      const x = (L + k * B + i) * 2 + c;
      peak = Math.max(peak, Math.abs(ref[r]));
      if (rtBits[x] !== refBits[r]) {
        same = false;
        if (rt[x] !== 0) {
          onlyWhereSilent = false;
          maxDiff = Math.max(maxDiff, Math.abs(rt[x] - ref[r]));
        }
      }
    }
  }
  if (same) identical++;
  else if (onlyWhereSilent) silentInRealtime++;
  else {
    mismatched++;
    if (firstMismatch < 0) firstMismatch = k;
  }
}
const summary = { plugin, latency: L, block: B, blocks, identical, silentInRealtime, mismatched, firstMismatch,
  maxDiffInMismatches: maxDiff, referencePeak: peak, replay: report, seconds: (Date.now() - started) / 1000 };
writeFileSync(join(dir, "identity.json"), JSON.stringify(summary, null, 2));
console.log(JSON.stringify(summary));
process.exit(mismatched === 0 && silentInRealtime === 0 && identical === blocks && blocks > 0 ? 0 : 1);
