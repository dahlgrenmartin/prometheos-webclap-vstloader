// Records the buzz engine's master output (worklet output 0, already scaled to
// +/-1) for the Windows VST browser checks: `frames` frames of stereo, then
// posts them. Allocates only when a recording is armed, never while recording.
class HarnessRecorder extends AudioWorkletProcessor {
  constructor() {
    super();
    this.buffer = null;
    this.at = 0;
    this.port.onmessage = (e) => {
      if (e.data.type === "record") {
        this.buffer = new Float32Array(e.data.frames * 2);
        this.at = 0;
      }
    };
  }

  process(inputs) {
    const input = inputs[0];
    if (!this.buffer || !input || input.length === 0) return true;
    const left = input[0];
    const right = input[1] || input[0];
    const frames = this.buffer.length / 2;
    const n = Math.min(left.length, frames - this.at);
    for (let i = 0; i < n; i++) {
      this.buffer[(this.at + i) * 2] = left[i];
      this.buffer[(this.at + i) * 2 + 1] = right[i];
    }
    this.at += n;
    if (this.at >= frames) {
      const done = this.buffer;
      this.buffer = null;
      this.port.postMessage({ type: "recorded", samples: done }, [done.buffer]);
    }
    return true;
  }
}

registerProcessor("harness-recorder", HarnessRecorder);
