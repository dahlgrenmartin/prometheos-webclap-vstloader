// Renders plugins in headless Chromium through the demo page, one emulator boot
// for all of them, and checks each result:
//   node tests/browser_render.mjs <base url> <screenshot dir> <plugin file>...
import { chromium } from "playwright";

const [base, shots, ...plugins] = process.argv.slice(2);
const browser = await chromium.launch({ args: ["--autoplay-policy=no-user-gesture-required"] });
const page = await browser.newPage({ viewport: { width: 1100, height: 1000 } });
page.on("pageerror", (e) => console.log("pageerror:", e.message));
await page.goto(`${base}/index.html`);
await page.waitForSelector("#plugin option", { state: "attached" });
let failures = 0;
for (const plugin of plugins) {
  await page.evaluate(() => { window.vstPocResult = undefined; });
  await page.selectOption("#plugin", plugin);
  const started = Date.now();
  await page.click("#render");
  const result = await page
    .waitForFunction(() => window.vstPocResult, null, { timeout: 25 * 60 * 1000, polling: 1000 })
    .then((h) => h.jsonValue());
  const r = result.report || {};
  const ok = Boolean(r.ok && r.peak > 0.01 && r.nonFinite === 0);
  failures += ok ? 0 : 1;
  console.log(JSON.stringify({
    plugin, ok, seconds: (Date.now() - started) / 1000, boot: result.bootSeconds, error: result.error,
    name: r.name, format: r.format, peak: r.peak, rms: r.rms, nonFinite: r.nonFinite,
    renderMs: r.renderMs, realtimeFactor: r.realtimeFactor, params: (r.params || []).length,
  }));
  if (shots) await page.screenshot({ path: `${shots}/${plugin.replace(/\W+/g, "_")}.png`, fullPage: true });
}
await browser.close();
process.exit(failures ? 1 : 0);
