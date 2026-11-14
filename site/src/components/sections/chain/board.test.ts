import assert from "node:assert/strict";
import { test } from "node:test";
import { initialBoard, isHome, lit, reduce, type Action, type Board } from "./board.ts";
import type { Stage } from "./signal.ts";

const HOME: readonly Stage[] = ["gate", "comp", "drive", "amp", "cab", "eq", "mod", "delay", "reverb"];
const run = (actions: Action[], from: Board = initialBoard(HOME)) => actions.reduce((b, a) => reduce(b, a, HOME), from);

test("the demonstration swaps drive and reverb and says what that does", () => {
  const b = run([{ type: "demo" }]);
  assert.deepEqual(b.order, ["gate", "comp", "reverb", "amp", "cab", "eq", "mod", "delay", "drive"]);
  assert.deepEqual(b.note, { kind: "tail" });
  assert.deepEqual(lit(b.phase), ["drive", "reverb"]);
  assert.deepEqual(run([{ type: "demoEnd" }], b).phase, { kind: "idle" });
});

test("the demonstration plays once and never over the visitor's own change", () => {
  const twice = run([{ type: "demo" }, { type: "demoEnd" }, { type: "demo" }]);
  assert.equal(twice.order[2], "reverb");
  assert.deepEqual(twice.phase, { kind: "idle" });

  const touched = run([{ type: "step", id: "gate", by: 1 }, { type: "demo" }]);
  assert.deepEqual(touched.order.slice(0, 3), ["comp", "gate", "drive"]);

  const holding = run([{ type: "grab", id: "amp" }, { type: "demo" }]);
  assert.deepEqual(holding.order, HOME);
  assert.deepEqual(holding.phase, { kind: "dragging", id: "amp" });
});

test("a drag keeps the block in the hand until it is dropped", () => {
  const order: Stage[] = ["gate", "comp", "amp", "drive", "cab", "eq", "mod", "delay", "reverb"];
  const held = run([{ type: "grab", id: "drive" }, { type: "arrange", order, id: "drive" }]);
  assert.deepEqual(held.order, order);
  assert.deepEqual(held.phase, { kind: "dragging", id: "drive" });
  assert.deepEqual(lit(held.phase), ["drive"]);
  assert.deepEqual(held.note, { kind: "after", id: "drive", other: "amp" });
  assert.deepEqual(run([{ type: "drop" }], held).phase, { kind: "idle" });
});

test("a step moves one place and stops at either end", () => {
  const first = run([{ type: "step", id: "comp", by: -1 }]);
  assert.deepEqual(first.order.slice(0, 2), ["comp", "gate"]);
  assert.deepEqual(first.note, { kind: "first", id: "comp" });

  const stuck = run([{ type: "step", id: "gate", by: -1 }]);
  assert.deepEqual(stuck.order, HOME);
  assert.deepEqual(stuck.note, { kind: "rest" });
  assert.deepEqual(run([{ type: "step", id: "reverb", by: 1 }]).order, HOME);
});

test("the tail line is said when a move puts the reverb ahead of the drive, not after it", () => {
  const moves: Action[] = Array.from({ length: 6 }, () => ({ type: "step", id: "drive", by: 1 }) as const);
  const b = run(moves);
  assert.equal(b.order.at(-1), "drive");
  assert.deepEqual(b.note, { kind: "tail" });
  assert.deepEqual(run([{ type: "step", id: "gate", by: 1 }], b).note, { kind: "after", id: "gate", other: "comp" });
});

test("reset and a move back home both return to the default order", () => {
  const b = run([{ type: "demo" }, { type: "demoEnd" }, { type: "reset" }]);
  assert.ok(isHome(b, HOME));
  assert.deepEqual(b.note, { kind: "rest" });
  assert.deepEqual(b.phase, { kind: "idle" });

  const back = run([{ type: "step", id: "gate", by: 1 }, { type: "step", id: "gate", by: -1 }]);
  assert.ok(isHome(back, HOME));
  assert.deepEqual(back.note, { kind: "rest" });
});
