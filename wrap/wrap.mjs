// Wraps a 32-bit Windows VST2 plugin into a WebCLAP bundle for any host that
// supports prometheos.runtime/1:
//   node wrap/wrap.mjs <plugin.dll> --site <url serving a built site>
//        [--runtime <runtime page URL in the host>] [--out <Name.wclap>]
// The plugin is described once in the runtime (headless Chromium: the site's
// runtime/index.html, which boots Boxedwine), and the bundle gets the shim
// (build/vstloader.wasm), the binary and the frozen descriptor:
//   module.wasm, resources/plugin.dll, resources/vstloader.txt
// --runtime is the URL the host loads the runtime from (it must serve it on
// its own origin); a relative URL resolves against the host page.
// CHROMIUM=/path/to/chrome uses a preinstalled browser.
import { execFileSync } from "node:child_process";
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const [dllPath, ...rest] = process.argv.slice(2);
if (!dllPath) {
  console.error("usage: node wrap/wrap.mjs <plugin.dll> --site <url> [--runtime <url>] [--out <file.wclap>]");
  process.exit(2);
}
const opt = { site: "", runtime: "/vstloader/runtime/index.html", out: "", wasm: join(root, "build", "vstloader.wasm") };
for (let i = 0; i < rest.length; i += 2) opt[rest[i].replace(/^--/, "")] = rest[i + 1];
if (!opt.site) throw new Error("--site is required (a served dist-mt/)");

const binary = readFileSync(dllPath);
if (binary.readUInt16LE(0) !== 0x5a4d) throw new Error(`${dllPath} is not a Windows binary`);
const pe = binary.readUInt32LE(0x3c);
if (binary.readUInt16LE(pe + 4) !== 0x14c) throw new Error(`${dllPath} is not a 32-bit (i386) binary; only 32-bit plugins run`);

const browser = await chromium.launch({ executablePath: process.env.CHROMIUM || undefined });
let result;
try {
  const page = await browser.newPage();
  page.on("console", (m) => /\[vstloader\]/.test(m.text()) && console.log(m.text()));
  await page.goto(`${opt.site.replace(/\/$/, "")}/runtime/index.html?boot=1`);
  await page.waitForFunction(() => window.vstloaderRuntime?.state.phase === "ready" || window.vstloaderRuntime?.state.phase === "failed", null, {
    timeout: 900000,
  });
  result = await page.evaluate(async (b64) => {
    const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
    return window.vstloaderRuntime.describeBinary(bytes);
  }, binary.toString("base64"));
} finally {
  await browser.close();
}

const { sha256, describe } = result;
const clean = (text) => String(text ?? "").replace(/[\t\r\n]/g, " ").trim();
const lines = [
  `id=prometheos.vstloader.${sha256.slice(0, 16)}`,
  `name=${clean(describe.name) || basename(dllPath, ".dll")}`,
  `vendor=${clean(describe.vendor)}`,
  `version=${describe.vendorVersion ? String(describe.vendorVersion) : "1.0.0"}`,
  `sha256=${sha256}`,
  `runtime=${opt.runtime}`,
  "dll=plugin.dll",
  `synth=${describe.synth ? 1 : 0}`,
  `inPorts=${describe.inPorts.length}`,
  `outPorts=${describe.outPorts.length}`,
  `latency=${Math.max(0, describe.latency | 0)}`,
  "bridgeLatency=2048",
  "block=256",
  ...describe.params.map((p) => `param=${clean(p.name)}\t${clean(p.label)}\t${Number(p.value).toFixed(6)}`),
];
const work = mkdtempSync(join(tmpdir(), "vstloader-wrap-"));
mkdirSync(join(work, "resources"));
copyFileSync(opt.wasm, join(work, "module.wasm"));
copyFileSync(dllPath, join(work, "resources", "plugin.dll"));
writeFileSync(join(work, "resources", "vstloader.txt"), `${lines.join("\n")}\n`);
const out = resolve(opt.out || `${basename(dllPath, ".dll")}.wclap`);
rmSync(out, { force: true });
execFileSync("zip", ["-q", "-X", "-0", "-r", out, "module.wasm", "resources"], { cwd: work });
rmSync(work, { recursive: true, force: true });
console.log(JSON.stringify({ out, sha256, name: describe.name, params: describe.params.length, synth: describe.synth,
  inPorts: describe.inPorts.length, outPorts: describe.outPorts.length, latency: describe.latency }));
