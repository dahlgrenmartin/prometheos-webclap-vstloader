// The browser wrapper (wrap.html): a plugin binary in, a .wclap.tar.gz out,
// described by this site's runtime in a hidden frame. The Node wrapper
// (wrap/wrap.mjs) builds the same bundle through wrap/bundle.js.
import { buildBundle, bundleFileName, checkBinary } from "./bundle.js";

const $ = (id) => document.getElementById(id);
const runtimeInput = $("runtime");
const logBox = $("log");
const result = $("result");
runtimeInput.value = new URL("index.html", location.href).href;

// The emulator needs cross-origin isolation. A host that cannot send COOP/COEP
// (GitHub Pages) gets it from the site's coi-sw.js: register it, then reload once under
// its control. The session flag stops a reload loop where that cannot work.
const RELOADED = "vstloader-coi-reload";
async function isolate() {
  if (crossOriginIsolated) {
    sessionStorage.removeItem(RELOADED);
    $("runtimeFrame").src = "index.html?boot=1";
    return;
  }
  if (!("serviceWorker" in navigator) || !isSecureContext) {
    say("This browser cannot isolate the page here (no service workers on this origin), so the emulator cannot run.");
    return;
  }
  if (sessionStorage.getItem(RELOADED)) {
    say("This page is still not cross-origin isolated after enabling it, so the emulator cannot run. " +
      "Serve it with COOP: same-origin and COEP: require-corp.");
    return;
  }
  say("Enabling cross-origin isolation…");
  await navigator.serviceWorker.register("../coi-sw.js", { scope: "../" });
  await navigator.serviceWorker.ready;
  sessionStorage.setItem(RELOADED, "1");
  location.reload();
}
isolate().catch((error) => say(`Error: could not enable cross-origin isolation: ${error?.message ?? error}`));

function say(text) {
  logBox.textContent += `${logBox.textContent ? "\n" : ""}${text}`;
}

async function runtime() {
  const frame = $("runtimeFrame");
  for (;;) {
    const r = frame.contentWindow?.vstloaderRuntime;
    if (r?.state.phase === "ready") return r;
    if (r?.state.phase === "failed") throw new Error(`the emulator failed to start: ${r.state.error}`);
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
}

let busy = false;
async function wrap(file) {
  if (busy) return;
  busy = true;
  result.textContent = "";
  logBox.textContent = "";
  try {
    if (!crossOriginIsolated) throw new Error("this page is not cross-origin isolated (COOP/COEP), so the emulator cannot run");
    const plugin = new Uint8Array(await file.arrayBuffer());
    const format = checkBinary(plugin, file.name);
    say(`${file.name}: 32-bit ${format === "vst3" ? "VST3" : "VST2"}, ${(plugin.length / 1024).toFixed(0)} KiB`);
    const started = performance.now();
    say("Starting the emulator…");
    const r = await runtime();
    say("Loading the plugin in Wine…");
    const { sha256, describe } = await r.describeBinary(plugin);
    say(`${describe.name || file.name} by ${describe.vendor || "unknown"}: ${describe.synth ? "instrument" : "effect"}, ` +
      `${describe.params.length} parameters, ${describe.inPorts.length} in / ${describe.outPorts.length} out` +
      (describe.latency ? `, ${describe.latency} samples latency` : ""));
    const wasm = new Uint8Array(await (await fetch("vstloader.wasm")).arrayBuffer());
    const bundle = await buildBundle({ wasm, plugin, describe, sha256, runtime: runtimeInput.value.trim(), fileName: file.name });
    const name = bundleFileName(describe, file.name);
    const link = document.createElement("a");
    link.className = "download";
    link.href = URL.createObjectURL(new Blob([bundle], { type: "application/gzip" }));
    link.download = name;
    link.textContent = `Download ${name}`;
    result.append(link);
    say(`Done in ${((performance.now() - started) / 1000).toFixed(1)} s. Install it in buzz-remote from Plugins…`);
  } catch (error) {
    say(`Error: ${error?.message ?? error}`);
  } finally {
    busy = false;
  }
}

$("file").addEventListener("change", (event) => {
  const file = event.target.files?.[0];
  if (file) void wrap(file);
});
const drop = $("drop");
drop.addEventListener("dragover", (event) => {
  event.preventDefault();
  drop.classList.add("over");
});
drop.addEventListener("dragleave", () => drop.classList.remove("over"));
drop.addEventListener("drop", (event) => {
  event.preventDefault();
  drop.classList.remove("over");
  const file = event.dataTransfer?.files?.[0];
  if (file) void wrap(file);
});

/** For tests: wraps bytes as if dropped and returns the bundle. */
window.vstloaderWrap = async (bytes, fileName) => {
  await wrap(new File([bytes], fileName));
  const link = result.querySelector("a");
  if (!link) throw new Error(logBox.textContent);
  return { name: link.download, bytes: new Uint8Array(await (await fetch(link.href)).arrayBuffer()), log: logBox.textContent };
};
