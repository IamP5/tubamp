import assert from "node:assert/strict";
import { test } from "node:test";
import { loadOrder, pickTier } from "./plan.ts";

const seq = {
  base: "/film",
  count: 121,
  poster: "/film.jpg",
  tiers: [
    { width: 1920, height: 1080 },
    { width: 2560, height: 1440 },
  ],
};

test("every frame is loaded exactly once", () => {
  const order = loadOrder(121);
  assert.equal(order.length, 121);
  assert.deepEqual([...order].sort((a, b) => a - b), Array.from({ length: 121 }, (_, i) => i));
});

test("the first frames to load are spread across the whole film", () => {
  const first = loadOrder(121).slice(0, 8);
  assert.deepEqual(first, [0, 16, 32, 48, 64, 80, 96, 112]);
});

test("a short film still loads all of its frames", () => {
  assert.deepEqual(loadOrder(3), [0, 2, 1]);
});

test("a tall retina box gets the large frames", () => {
  assert.equal(pickTier(seq, 1800, false).width, 2560);
});

test("a box the small frames can fill gets the small frames", () => {
  assert.equal(pickTier(seq, 1100, false).width, 1920);
});

test("a box taller than every tier gets the largest", () => {
  assert.equal(pickTier(seq, 4000, false).width, 2560);
});

test("a constrained connection gets the small frames whatever the box", () => {
  assert.equal(pickTier(seq, 1800, true).width, 1920);
});
