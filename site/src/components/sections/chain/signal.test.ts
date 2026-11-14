import assert from "node:assert/strict";
import { test } from "node:test";
import { DRIVE_CLIP, RATE, drySignal, noteEnvelope, renderSignal, type Stage } from "./signal.ts";

const win = { start: -137, length: RATE };
const peak = (x: Float32Array) => x.reduce((m, v) => Math.max(m, Math.abs(v)), 0);

/** Sample indices where every note has died away: only noise is left. */
const gaps = Array.from({ length: win.length }, (_, i) => i).filter(
  (i) => noteEnvelope((win.start + i) / RATE) < 0.01,
);

test("an empty chain is the dry signal, unchanged", () => {
  assert.deepEqual(renderSignal([], win), drySignal(win));
});

test("drive never lets a sample past the clip level, however hot the input", () => {
  const chains: Stage[][] = [["drive"], ["comp", "drive"], ["gate", "comp", "drive"], ["amp", "comp", "drive"]];
  for (const chain of chains) {
    const before = renderSignal(chain.slice(0, -1), win);
    assert.ok(chain.length === 1 || peak(before) > DRIVE_CLIP, `input to drive is hotter than the clip in ${chain}`);
    assert.ok(peak(renderSignal(chain, win)) <= DRIVE_CLIP, `clipped in ${chain}`);
  }
});

test("the gate takes the floor between notes to zero", () => {
  assert.ok(gaps.length > RATE * 0.1, "the window has gaps between notes");
  const dry = drySignal(win);
  assert.ok(Math.max(...gaps.map((i) => Math.abs(dry[i]))) > 0.02, "the dry gaps carry noise");
  const gated = renderSignal(["gate"], win);
  for (const i of gaps) assert.equal(Math.abs(gated[i]), 0, `sample ${i} is silent`);
  assert.ok(peak(gated) > 0.5, "the notes themselves pass");
});

test("order is part of the sound", () => {
  const chain: Stage[] = ["gate", "comp", "drive", "amp", "cab", "eq", "mod", "delay", "reverb"];
  const swapped: Stage[] = ["gate", "comp", "reverb", "amp", "cab", "eq", "mod", "delay", "drive"];
  const a = renderSignal(chain, win);
  const b = renderSignal(swapped, win);
  const diff = a.reduce((s, v, i) => s + Math.abs(v - b[i]), 0) / a.length;
  assert.ok(diff > 0.05, `mean difference ${diff}`);
});
