// Boots Boxedwine in headless Chromium with one guest program and prints the
// console lines that match a filter, until a line matches the stop pattern:
//   node tests/run_guest.mjs <base url> <program> [--args "..."] [--grep re] [--until re] [--timeout s]
// CHROMIUM=/path/to/chrome uses a preinstalled browser.
import { chromium } from "playwright";

const [base, program, ...rest] = process.argv.slice(2);
const opt = { args: "", grep: ".", until: "$^", timeout: 900 };
for (let i = 0; i < rest.length; i += 2) opt[rest[i].replace(/^--/, "")] = rest[i + 1];
const grep = new RegExp(opt.grep);
const until = new RegExp(opt.until);

const browser = await chromium.launch({ executablePath: process.env.CHROMIUM || undefined });
const page = await browser.newPage();
const started = Date.now();
const stamp = () => ((Date.now() - started) / 1000).toFixed(1).padStart(6);
let done;
const finished = new Promise((resolve) => (done = resolve));
page.on("console", (msg) => {
  // Wine's console wraps guest stdout in terminal escapes; drop them.
  const text = msg.text().replace(/\x1b\[1C/g, " ").replace(/\x1b\[[0-9;?]*[A-Za-z]/g, "");
  if (grep.test(text)) console.log(`${stamp()} ${text}`);
  if (until.test(text)) done(true);
});
page.on("pageerror", (e) => console.log(`${stamp()} pageerror: ${e.message}`));
const params = new URLSearchParams({
  root: "vstpoc-prefix",
  overlay: "TinyCore15Wine11.0",
  app: "vstpoc.zip",
  p: program,
  args: opt.args,
  storage: "memory",
  sound: "false",
});
await page.goto(`${base}/boxedwine/boxedwine.html?${params}`);
const ok = await Promise.race([finished, new Promise((r) => setTimeout(() => r(false), opt.timeout * 1000))]);
console.log(`${stamp()} ${ok ? "finished" : "timed out"}`);
await browser.close();
process.exit(ok ? 0 : 1);
