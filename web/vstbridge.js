// Host side of Boxedwine's /dev/vstbridge (layout: vstbridge-abi.js). Used by
// the page (control requests), its AudioWorklet (streaming) and a benchmark
// worker. Plain ES module, no allocation in the streaming path.
import { VSTB } from "./vstbridge-abi.js";

const C = VSTB.constants;
const E = VSTB.enums;
const CTL = VSTB.channel_ctl;
const REQ = VSTB.request;
const EV = VSTB.event;

export { VSTB };

/** Views on one region inside the emulator's shared WebAssembly memory. */
export class BridgeRegion {
  constructor(buffer, base) {
    this.buffer = buffer;
    this.base = base;
    this.header = new Int32Array(buffer, base, VSTB.region_header.size / 4);
    this.ctrl = new Int32Array(buffer, base + C.CTRL_OFFSET, VSTB.ctrl_header.size / 4);
    this.bytes = new Uint8Array(buffer, base, C.REGION_BYTES);
  }

  get valid() {
    return (this.header[0] >>> 0) === C.MAGIC && this.header[1] === C.VERSION;
  }

  get serving() {
    return Atomics.load(this.header, VSTB.region_header.bridgeState / 4) === E.BRIDGE_SERVING;
  }

  channel(n) {
    return new BridgeChannel(this, n);
  }
}

/** Views on one audio channel. */
export class BridgeChannel {
  constructor(region, n) {
    const base = region.base + C.CHANNELS_OFFSET + (n - 1) * C.CHANNEL_BYTES;
    const buffer = region.buffer;
    this.n = n;
    this.ctl = new Int32Array(buffer, base, C.CHANNEL_CTL_BYTES / 4);
    this.slots = new Uint8Array(buffer, base + C.SLOTS_OFFSET, C.SLOTS * C.REQUEST_BYTES);
    this.inRings = [];
    this.outRings = [];
    for (let i = 0; i < C.MAX_IN_PORTS * 2; i++) {
      this.inRings.push(new Float32Array(buffer, base + C.IN_RINGS_OFFSET + i * C.RING_FRAMES * 4, C.RING_FRAMES));
    }
    for (let i = 0; i < C.MAX_OUT_PORTS * 2; i++) {
      this.outRings.push(new Float32Array(buffer, base + C.OUT_RINGS_OFFSET + i * C.RING_FRAMES * 4, C.RING_FRAMES));
    }
  }
  word(field) {
    return Atomics.load(this.ctl, CTL[field] / 4);
  }
  doneBlock(k) {
    return Atomics.load(this.ctl, CTL.doneBlock / 4 + (k % C.SLOTS));
  }
  turnUs(k) {
    return Atomics.load(this.ctl, CTL.turnUs / 4 + (k % C.SLOTS));
  }
  histogram(field) {
    const out = [];
    for (let i = 0; i < C.HIST_BUCKETS; i++) out.push(Atomics.load(this.ctl, CTL[field] / 4 + i));
    return out;
  }
}

/**
 * Builds request records for one channel and publishes them: the record is
 * assembled in private memory and copied into its slot only when published.
 */
export class RequestWriter {
  constructor(channel) {
    this.channel = channel;
    this.record = new ArrayBuffer(C.REQUEST_BYTES);
    this.bytes = new Uint8Array(this.record);
    this.view = new DataView(this.record);
    this.reset(0);
  }

  reset(blockIndex) {
    this.bytes.fill(0);
    this.view.setUint32(REQ.blockIndex, blockIndex, true);
    this.count = 0;
    this.dropped = 0;
  }

  midi(offset, status, data1, data2) {
    if (this.count >= C.MAX_EVENTS) return void this.dropped++;
    const at = REQ.events + this.count++ * EV.size;
    this.view.setUint32(at + EV.offset, offset, true);
    this.view.setUint16(at + EV.type, E.EV_MIDI, true);
    this.bytes[at + EV.midi] = status;
    this.bytes[at + EV.midi + 1] = data1;
    this.bytes[at + EV.midi + 2] = data2;
  }

  param(offset, index, value) {
    if (this.count >= C.MAX_EVENTS) return void this.dropped++;
    const at = REQ.events + this.count++ * EV.size;
    this.view.setUint32(at + EV.offset, offset, true);
    this.view.setUint16(at + EV.type, E.EV_PARAM, true);
    this.view.setUint16(at + EV.index, index, true);
    this.view.setFloat32(at + EV.value, value, true);
  }

  /** Copies the record into its slot and rings the doorbell. */
  publish(blockIndex, frames, { flags = 0, connectedMask = 0, tempo = 120, ppqPos = 0, samplePos = 0, hostTimeMs = 0 } = {}) {
    const v = this.view;
    v.setUint32(REQ.blockIndex, blockIndex, true);
    v.setUint32(REQ.frames, frames, true);
    v.setUint32(REQ.connectedMask, connectedMask, true);
    v.setUint32(REQ.flags, flags, true);
    v.setFloat64(REQ.tempo, tempo, true);
    v.setFloat64(REQ.ppqPos, ppqPos, true);
    v.setFloat64(REQ.samplePos, samplePos, true);
    v.setFloat64(REQ.hostTimeMs, hostTimeMs, true);
    v.setUint32(REQ.eventCount, this.count, true);
    v.setUint32(REQ.droppedEvents, this.dropped, true);
    const ch = this.channel;
    ch.slots.set(this.bytes, (blockIndex % C.SLOTS) * C.REQUEST_BYTES);
    Atomics.store(ch.ctl, CTL.requestSeq / 4, blockIndex + 1);
    Atomics.add(ch.ctl, CTL.doorbell / 4, 1);
    Atomics.notify(ch.ctl, CTL.doorbell / 4);
  }
}

/** Control requests from the page (main thread): one at a time. */
export class ControlClient {
  constructor(region) {
    this.region = region;
    this.queue = Promise.resolve();
  }

  /** Sends op with a string or byte payload; resolves {status, bytes, text}. */
  request(op, channel = 0, payload = "", timeoutMs = 600000) {
    const run = () => this.#send(op, channel, payload, timeoutMs);
    const result = this.queue.then(run, run);
    this.queue = result.catch(() => {});
    return result;
  }

  async #send(op, channel, payload, timeoutMs) {
    const r = this.region;
    // (created here: AudioWorkletGlobalScope, which imports this module too, has no TextEncoder)
    const data = typeof payload === "string" ? new TextEncoder().encode(payload) : payload;
    if (data.length > C.CTRL_BYTES - VSTB.msg.size) throw new Error("control payload too large");
    const seq = (Atomics.load(r.ctrl, VSTB.ctrl_header.requestSeq / 4) + 1) | 0;
    const at = r.base + C.CTRL_REQUEST_OFFSET;
    const view = new DataView(r.buffer, at, VSTB.msg.size);
    view.setUint32(VSTB.msg.seq, seq, true);
    view.setUint32(VSTB.msg.op, op, true);
    view.setUint32(VSTB.msg.channel, channel, true);
    view.setUint32(VSTB.msg.length, data.length, true);
    view.setInt32(VSTB.msg.status, 0, true);
    new Uint8Array(r.buffer, at + VSTB.msg.size, data.length).set(data);
    Atomics.store(r.ctrl, VSTB.ctrl_header.requestSeq / 4, seq);
    Atomics.notify(r.ctrl, VSTB.ctrl_header.requestSeq / 4);
    const deadline = performance.now() + timeoutMs;
    const index = VSTB.ctrl_header.responseSeq / 4;
    while (Atomics.load(r.ctrl, index) !== seq) {
      if (performance.now() > deadline) throw new Error(`control op ${op} timed out`);
      const current = Atomics.load(r.ctrl, index);
      if (Atomics.waitAsync) await Atomics.waitAsync(r.ctrl, index, current, 100).value;
      else await new Promise((resolve) => setTimeout(resolve, 5));
    }
    const out = r.base + C.CTRL_RESPONSE_OFFSET;
    const head = new DataView(r.buffer, out, VSTB.msg.size);
    const length = head.getUint32(VSTB.msg.length, true);
    const status = head.getInt32(VSTB.msg.status, true);
    const bytes = new Uint8Array(r.buffer, out + VSTB.msg.size, length).slice();
    return { status, bytes, text: new TextDecoder().decode(bytes) };
  }
}

/** Percentile of a histogram of log2 buckets (bucket i: [2^i, 2^(i+1)) us). */
export function histogramPercentile(buckets, p) {
  const total = buckets.reduce((a, b) => a + b, 0);
  if (!total) return 0;
  let seen = 0;
  for (let i = 0; i < buckets.length; i++) {
    seen += buckets[i];
    if (seen >= p * total) return 2 ** (i + 1);
  }
  return 2 ** buckets.length;
}
