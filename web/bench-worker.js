// Wake-up and per-block overhead benchmark (Phase 0, task 6): publishes one
// request at a time on a channel and waits for its response, with the publish
// time in the request so the device can histogram publish -> guest wake-up.
// A dedicated worker has the cross-thread clock (timeOrigin + now) that the
// AudioWorklet lacks, and may block in Atomics.wait.
import { BridgeRegion, RequestWriter, VSTB } from "./vstbridge.js";

onmessage = (e) => {
  const { buffer, base, channel, block, iterations, warmup } = e.data;
  const region = new BridgeRegion(buffer, base);
  const ch = region.channel(channel);
  const writer = new RequestWriter(ch);
  const now = () => performance.timeOrigin + performance.now();
  const seqIndex = VSTB.channel_ctl.responseSeq / 4;
  const rtt = [];
  let k = ch.word("requestSeq");
  for (let i = 0; i < warmup + iterations; i++, k++) {
    writer.reset(k);
    if (i % 50 === 0) writer.midi(0, 0x90, 60 + (i % 24), 100);
    if (i % 50 === 25) writer.midi(0, 0x80, 60 + ((i - 25) % 24), 0);
    const t0 = now();
    writer.publish(k, block, { hostTimeMs: t0 });
    while (ch.doneBlock(k) !== k + 1) {
      const seen = Atomics.load(ch.ctl, seqIndex);
      if (ch.doneBlock(k) === k + 1) break;
      if (Atomics.wait(ch.ctl, seqIndex, seen, 5000) === "timed-out") {
        postMessage({ error: `no response for block ${k}` });
        return;
      }
    }
    if (i >= warmup) rtt.push((now() - t0) * 1000);
  }
  rtt.sort((a, b) => a - b);
  const pct = (p) => rtt[Math.min(rtt.length - 1, Math.floor(p * rtt.length))];
  postMessage({
    iterations,
    roundTripUs: { p50: pct(0.5), p90: pct(0.9), p99: pct(0.99), max: rtt[rtt.length - 1], min: rtt[0] },
    wakeHist: ch.histogram("wakeHist"),
    turnHist: ch.histogram("turnHist"),
  });
};
