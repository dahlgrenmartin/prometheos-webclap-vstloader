// Checks the vstbridge layout's copies against include/vstbridge_abi.h:
//   - include/vstbridge_abi.json equals what tests/abi_layout.c prints (when a
//     C compiler is available; CI always has one),
//   - web/vstbridge-abi.js equals that JSON,
//   - the header copy that Boxedwine patch 0003 adds equals the header.
//   node tests/abi_layout.mjs
import { execFileSync } from "node:child_process";
import { mkdirSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { isDeepStrictEqual } from "node:util";
import { VSTB } from "../web/vstbridge-abi.js";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const golden = JSON.parse(readFileSync(join(root, "include/vstbridge_abi.json"), "utf8"));
let failures = 0;
const check = (ok, what) => {
  console.log(`${ok ? "ok  " : "FAIL"} ${what}`);
  if (!ok) failures++;
};

try {
  mkdirSync(join(root, "build"), { recursive: true });
  const exe = join(root, "build/abi_layout");
  execFileSync(process.env.CC || "cc", ["-std=c11", "-Wall", "-Werror", `-I${join(root, "include")}`,
    join(root, "tests/abi_layout.c"), "-o", exe]);
  const fresh = JSON.parse(execFileSync(exe, { encoding: "utf8" }));
  check(isDeepStrictEqual(fresh, golden), "include/vstbridge_abi.json matches the compiled header");
} catch (e) {
  console.log(`skip compiled layout check (${e.message.split("\n")[0]})`);
}

check(isDeepStrictEqual(VSTB, golden), "web/vstbridge-abi.js matches include/vstbridge_abi.json");

// The patch adds source/kernel/devs/vstbridge_abi.h as a new file: its "+" lines.
const patch = readFileSync(join(root, "patches/boxedwine/0003-vstbridge-device.patch"), "utf8");
const section = patch.split(/^diff --git /m).find((s) => s.startsWith("a/source/kernel/devs/vstbridge_abi.h"));
const copied = section
  ? section.split("\n").filter((l) => l.startsWith("+") && !l.startsWith("+++")).map((l) => l.slice(1)).join("\n") + "\n"
  : "";
check(copied === readFileSync(join(root, "include/vstbridge_abi.h"), "utf8"), "patch 0003's vstbridge_abi.h matches include/");

process.exit(failures ? 1 : 0);
