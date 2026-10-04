// One direction of one instance's stream between the vstloader WebCLAP's
// channel (in the plugin module's shared memory) and a /dev/vstbridge channel
// (in Boxedwine's). Both use the same layout (vstbridge_abi.h), so a block
// moves as plain copies at the same offsets.
//
//   forward:  waits on the plugin channel's doorbell; copies each published
//             request (slot + effect input) and publishes it to the bridge.
//   backward: waits on the bridge channel's responseSeq (the device wakes it
//             after stamping a block); copies the block's output and stamps
//             it done on the plugin channel.
//
// Blocking Atomics.wait on both sides: no event loop and no main thread on the
// audio path. Allocation-free once started.
import { VSTB } from "./vstbridge-abi.js";

const C = VSTB.constants;
const CTL = VSTB.channel_ctl;
const WAIT_MS = 100;

function views(buffer, base) {
  return {
    ctl: new Int32Array(buffer, base, C.CHANNEL_CTL_BYTES / 4),
    bytes: new Uint8Array(buffer, base, C.CHANNEL_BYTES),
    f32: new Float32Array(buffer, base, C.CHANNEL_BYTES / 4),
  };
}

self.onmessage = (event) => {
  const { mode, plugin, bridge, stop, block, inPorts, outPorts } = event.data;
  const p = views(plugin.buffer, plugin.base);
  const b = views(bridge.buffer, bridge.base);
  const stopFlag = new Int32Array(stop);
  if (mode === "forward") forward(p, b, stopFlag, block, inPorts);
  else backward(p, b, stopFlag, block, outPorts);
  self.postMessage({ stopped: mode });
};

function forward(p, b, stop, B, inPorts) {
  const slotsOffset = C.SLOTS_OFFSET;
  const inRings = C.IN_RINGS_OFFSET / 4;
  let next = Atomics.load(b.ctl, CTL.requestSeq / 4);
  while (Atomics.load(stop, 0) === 0) {
    const bell = Atomics.load(p.ctl, CTL.doorbell / 4);
    const published = Atomics.load(p.ctl, CTL.requestSeq / 4);
    while (next < published) {
      const slot = slotsOffset + (next % C.SLOTS) * C.REQUEST_BYTES;
      b.bytes.set(p.bytes.subarray(slot, slot + C.REQUEST_BYTES), slot);
      if (inPorts) {
        const at = (next * B) % C.RING_FRAMES;
        for (let c = 0; c < inPorts * 2; c++) {
          const i = inRings + c * C.RING_FRAMES + at;
          b.f32.set(p.f32.subarray(i, i + B), i);
        }
      }
      next++;
      Atomics.store(b.ctl, CTL.requestSeq / 4, next);
      Atomics.add(b.ctl, CTL.doorbell / 4, 1);
      Atomics.notify(b.ctl, CTL.doorbell / 4);
    }
    Atomics.wait(p.ctl, CTL.doorbell / 4, bell, WAIT_MS);
  }
}

function backward(p, b, stop, B, outPorts) {
  const outRings = C.OUT_RINGS_OFFSET / 4;
  let next = Atomics.load(p.ctl, CTL.responseSeq / 4);
  while (Atomics.load(stop, 0) === 0) {
    const answered = Atomics.load(b.ctl, CTL.responseSeq / 4);
    while (next < answered) {
      const slot = next % C.SLOTS;
      // A block the device skipped is not stamped; the plugin plays it as an underrun.
      if (Atomics.load(b.ctl, CTL.doneBlock / 4 + slot) === next + 1) {
        const at = (next * B) % C.RING_FRAMES;
        for (let c = 0; c < outPorts * 2; c++) {
          const i = outRings + c * C.RING_FRAMES + at;
          p.f32.set(b.f32.subarray(i, i + B), i);
        }
        Atomics.store(p.ctl, CTL.turnUs / 4 + slot, Atomics.load(b.ctl, CTL.turnUs / 4 + slot));
        Atomics.store(p.ctl, CTL.doneBlock / 4 + slot, next + 1);
      }
      next++;
    }
    Atomics.store(p.ctl, CTL.responseSeq / 4, next);
    Atomics.store(p.ctl, CTL.skipped / 4, Atomics.load(b.ctl, CTL.skipped / 4));
    Atomics.store(p.ctl, CTL.maxProcessUs / 4, Atomics.load(b.ctl, CTL.maxProcessUs / 4));
    Atomics.wait(b.ctl, CTL.responseSeq / 4, answered, WAIT_MS);
  }
}
