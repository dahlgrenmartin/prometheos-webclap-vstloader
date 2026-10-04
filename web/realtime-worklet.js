// Streams one vstbridge channel in real time: every block of B frames becomes
// a request (events with frame offsets, plus inputs), and the output played at
// frame t is the plugin's output for frame t - L. Nothing on this path touches
// the browser main thread: requests wake the emulator thread through
// Atomics.notify on the shared WebAssembly memory.
import { BridgeRegion, RequestWriter, VSTB } from "./vstbridge.js";

const C = VSTB.constants;
const PROCESS_BINS = 2000; // 25 us bins up to 50 ms

// Deterministic test material: a new 8-note chord every `period` frames
// (slightly off the block grid), held for 80 % of the period.
export class ChordSequencer {
  constructor(rate) {
    this.period = Math.round(rate * 0.5);
    this.hold = Math.round(this.period * 0.8);
  }
  start(i) {
    return i * this.period + ((i * 7919) % 997);
  }
  notes(i) {
    const base = 36 + ((i * 5) % 12);
    return [0, 7, 12, 16, 19, 24, 28, 31].map((d) => base + d);
  }
  velocity(i) {
    return 60 + ((i * 13) % 60);
  }
  // Calls emit(frame, status, data1, data2) for events in [a, b), in frame order.
  events(a, b, emit) {
    const first = Math.max(0, Math.floor((a - this.hold - 1000) / this.period));
    const last = Math.floor(b / this.period) + 1;
    for (let i = first; i <= last; i++) {
      const off = this.start(i - 1) + this.hold;
      if (i > 0 && off >= a && off < b) for (const n of this.notes(i - 1)) emit(off, 0x80, n, 0);
      const on = this.start(i);
      if (on >= a && on < b) for (const n of this.notes(i)) emit(on, 0x90, n, this.velocity(i));
    }
  }
}

class VstBridgeStream extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions;
    this.region = new BridgeRegion(o.buffer, o.base);
    this.ch = this.region.channel(o.channel);
    this.writer = new RequestWriter(this.ch);
    this.B = o.block;
    this.L = o.latency;
    this.inPorts = o.inPorts;
    this.outPorts = o.outPorts;
    this.connectedMask = o.connectedMask ?? (1 << o.inPorts) - 1;
    this.sequencer = o.sequence ? new ChordSequencer(sampleRate) : null;
    // Streams continue a channel's block sequence: the first block is the
    // channel's next request, and frame numbers are block index * B.
    this.k0 = o.startBlock || 0;
    this.startFrame = this.k0 * this.B;
    this.frame = this.startFrame; // input time
    this.k = this.k0; // block being assembled
    this.writer.reset(this.k);
    this.live = [];
    this.running = true;
    this.stats = {
      frames: 0, underruns: 0, underrunBlocks: 0, blocksPlayed: 0, minMarginFrames: Infinity, published: 0,
      eventsSent: 0, dropped: 0,
    };
    this.processHist = new Int32Array(PROCESS_BINS);
    this.lastPlayedBlock = -1;
    this.lastUnderrunBlock = -1;
    this.reportEvery = Math.round(sampleRate / 4);
    this.sinceReport = 0;
    // Capture for the sample-identity test: requests + inputs of the first
    // captureBlocks blocks, and the played output of the first captureFrames.
    const blocks = Math.ceil(((o.captureSeconds || 0) * sampleRate) / this.B);
    this.captureBlocks = blocks;
    this.recordBytes = C.REQUEST_BYTES + this.inPorts * 2 * this.B * 4;
    this.captureReq = blocks ? new Uint8Array(blocks * this.recordBytes) : null;
    this.captureFrames = blocks ? blocks * this.B + this.L : 0;
    this.captureOut = blocks ? new Float32Array(this.captureFrames * 2) : null;
    this.captureSent = false;
    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.type === "note") this.live.push(m.on ? 0x90 : 0x80, m.note, m.on ? m.velocity ?? 100 : 0);
      else if (m.type === "stop") this.running = false;
    };
  }

  addEvent(frame, status, d1, d2) {
    this.writer.midi(frame - this.k * this.B, status, d1, d2);
    this.stats.eventsSent++;
  }

  // Input side: frames [a, b) lie inside block this.k.
  stream(a, b, input) {
    if (this.sequencer) this.sequencer.events(a, b, (f, s, d1, d2) => this.addEvent(f, s, d1, d2));
    if (this.live.length) {
      for (let i = 0; i < this.live.length; i += 3) this.addEvent(a, this.live[i], this.live[i + 1], this.live[i + 2]);
      this.live.length = 0;
    }
    if (this.inPorts) {
      const at = a % C.RING_FRAMES;
      for (let c = 0; c < this.inPorts * 2; c++) {
        const src = input && input[c % (input.length || 1)];
        const ring = this.ch.inRings[c];
        for (let f = 0; f < b - a; f++) ring[at + f] = src ? src[f + (a - this.frame)] : 0;
      }
    }
    if (b === (this.k + 1) * this.B) {
      const k = this.k;
      this.stats.dropped += this.writer.dropped;
      this.writer.publish(k, this.B, {
        flags: VSTB.enums.REQ_PLAYING, connectedMask: this.connectedMask, tempo: 120,
        ppqPos: ((k * this.B) / sampleRate) * 2, samplePos: k * this.B,
      });
      if (k - this.k0 < this.captureBlocks) {
        // Exactly what was published: the record and the block's inputs.
        const rec = (k - this.k0) * this.recordBytes;
        this.captureReq.set(this.writer.bytes, rec);
        const at = (k * this.B) % C.RING_FRAMES;
        for (let c = 0; c < this.inPorts * 2; c++) {
          const ring = this.ch.inRings[c];
          const bytes = new Uint8Array(ring.buffer, ring.byteOffset + at * 4, this.B * 4);
          this.captureReq.set(bytes, rec + C.REQUEST_BYTES + c * this.B * 4);
        }
      }
      this.stats.published++;
      this.k = k + 1;
      this.writer.reset(this.k);
    }
  }

  // Output side: plays frames [t - L, t - L + n) into out.
  play(t, n, out) {
    const left = out[0], right = out[1] || out[0];
    let f = 0;
    while (f < n) {
      const x = t - this.L + f;
      if (x < this.startFrame) {
        const m = Math.min(n - f, this.startFrame - x);
        left.fill(0, f, f + m);
        right.fill(0, f, f + m);
        f += m;
        continue;
      }
      const j = Math.floor(x / this.B);
      const m = Math.min(n - f, (j + 1) * this.B - x);
      if (this.ch.doneBlock(j) === j + 1) {
        const at = x % C.RING_FRAMES;
        left.set(this.ch.outRings[0].subarray(at, at + m), f);
        right.set(this.ch.outRings[1].subarray(at, at + m), f);
        if (j !== this.lastPlayedBlock) {
          this.lastPlayedBlock = j;
          this.stats.blocksPlayed++;
          const us = this.ch.turnUs(j);
          this.processHist[Math.min(PROCESS_BINS - 1, Math.floor(us / 25))]++;
        }
      } else {
        left.fill(0, f, f + m);
        right.fill(0, f, f + m);
        this.stats.underruns += m;
        if (j !== this.lastUnderrunBlock) {
          this.lastUnderrunBlock = j;
          this.stats.underrunBlocks++;
          Atomics.add(this.ch.ctl, VSTB.channel_ctl.underruns / 4, 1);
        }
      }
      f += m;
    }
    // Margin: frames already rendered beyond what this quantum needed.
    if (t - this.L > this.startFrame) {
      const margin = this.ch.word("responseSeq") * this.B - (t - this.L + n);
      if (margin < this.stats.minMarginFrames) this.stats.minMarginFrames = margin;
    }
    const rel = t - this.startFrame;
    if (this.captureOut && rel < this.captureFrames) {
      const m = Math.min(n, this.captureFrames - rel);
      for (let i = 0; i < m; i++) {
        this.captureOut[(rel + i) * 2] = left[i];
        this.captureOut[(rel + i) * 2 + 1] = right[i];
      }
      if (rel + n >= this.captureFrames && !this.captureSent) {
        this.captureSent = true;
        this.port.postMessage({ type: "capture", requests: this.captureReq.buffer, output: this.captureOut.buffer,
          blocks: this.captureBlocks, recordBytes: this.recordBytes, frames: this.captureFrames },
        [this.captureReq.buffer, this.captureOut.buffer]);
        this.captureReq = null;
        this.captureOut = null;
      }
    }
  }

  report() {
    const total = this.processHist.reduce((a, b) => a + b, 0);
    const pct = (p) => {
      let seen = 0;
      for (let i = 0; i < PROCESS_BINS; i++) {
        seen += this.processHist[i];
        if (seen >= p * total) return (i + 1) * 25;
      }
      return PROCESS_BINS * 25;
    };
    let max = 0;
    for (let i = PROCESS_BINS - 1; i >= 0; i--) if (this.processHist[i]) { max = (i + 1) * 25; break; }
    this.port.postMessage({
      type: "stats", ...this.stats, seconds: (this.frame - this.startFrame) / sampleRate,
      processUs: total ? { p50: pct(0.5), p90: pct(0.9), p99: pct(0.99), p999: pct(0.999), max } : null,
      blockUs: (this.B / sampleRate) * 1e6, skipped: this.ch.word("skipped"), maxProcessUs: this.ch.word("maxProcessUs"),
      wakeHist: this.ch.histogram("wakeHist"), turnHist: this.ch.histogram("turnHist"),
    });
  }

  process(inputs, outputs) {
    if (!this.running) return false;
    const out = outputs[0];
    const n = out[0].length;
    const t = this.frame;
    // Input time: split at block boundaries.
    for (let a = t; a < t + n; ) {
      const b = Math.min(t + n, (Math.floor(a / this.B) + 1) * this.B);
      this.stream(a, b, inputs[0]);
      a = b;
    }
    this.play(t, n, out);
    this.frame = t + n;
    this.stats.frames = this.frame - this.startFrame;
    this.sinceReport += n;
    if (this.sinceReport >= this.reportEvery) {
      this.sinceReport = 0;
      this.report();
    }
    return true;
  }
}

registerProcessor("vstbridge-stream", VstBridgeStream);
