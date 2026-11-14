import type { Stage } from "./signal";

/**
 * The chain chapter as a function of scroll. `u` is the distance scrolled since
 * the chapter's top reached 80% of the viewport, in viewport heights. Every
 * frame of the chapter is `sceneAt(u)`, so scrubbing back and forth, a resize or
 * a jump to an anchor all land on the same picture.
 */

/** Scroll each act costs, in viewport heights. */
export const ACTS = { enter: 0.8, draw: 0.45, block: 0.36, swap: 0.75, hold: 0.35 } as const;

export const BLOCKS = 9;
export const SWAP = [2, 8] as const;

export const PINNED = ACTS.draw + BLOCKS * ACTS.block + ACTS.swap + ACTS.hold;
export const TOTAL = ACTS.enter + PINNED;

export type Tile = {
  /** 1 while still above its slot, 0 once landed. Already eased. */
  drop: number;
  opacity: number;
};

export type Scene = {
  /** 0..1 arrival of the label, headline and paragraph. */
  header: number;
  /** Where the wire ends, as an index into the wire's stops: 0 is the In jack, 1 + k is the open slot after k blocks. */
  wire: number;
  /** 0..1 fade of the In and Out jacks and the waveform. */
  chrome: number;
  tiles: Tile[];
  /** 0..1 progress of Drive and Reverb trading places. Already eased. */
  swap: number;
  wave: { from: Stage[]; to: Stage[]; mix: number };
  /** -1 for none, 0..8 the block just added, 9 the swap. */
  caption: number;
  /** Signal time shown, in units of signal; moves only with the scroll. */
  time: number;
};

const clamp01 = (v: number) => (v < 0 ? 0 : v > 1 ? 1 : v);
const span = (v: number, a: number, b: number) => clamp01((v - a) / (b - a));
const expoOut = (t: number) => (t >= 1 ? 1 : 1 - Math.pow(2, -10 * t));
const inOut = (t: number) => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2);
const smooth = (t: number) => t * t * (3 - 2 * t);

export function swapped(order: readonly Stage[]): Stage[] {
  const out = [...order];
  [out[SWAP[0]], out[SWAP[1]]] = [out[SWAP[1]], out[SWAP[0]]];
  return out;
}

export function sceneAt(u: number, order: readonly Stage[]): Scene {
  const header = span(u, 0, ACTS.enter);
  let t = u - ACTS.enter;

  const draw = span(t, 0, ACTS.draw);
  t -= ACTS.draw;

  const tiles: Tile[] = [];
  let wire = smooth(span(draw, 0.1, 0.75));
  let wave: Scene["wave"] = { from: [], to: [], mix: 1 };
  let caption = -1;

  for (let k = 0; k < BLOCKS; k++) {
    const f = span(t - k * ACTS.block, 0, ACTS.block);
    const fly = span(f, 0, 0.55);
    tiles.push({ drop: 1 - expoOut(fly), opacity: span(fly, 0, 0.25) });
    if (f > 0) {
      wire = 1 + k + inOut(span(f, 0.6, 0.95));
      wave = { from: order.slice(0, k), to: order.slice(0, k + 1), mix: smooth(span(f, 0.3, 0.7)) };
      if (f >= 0.35) caption = k;
    }
  }
  t -= BLOCKS * ACTS.block;

  const s = span(t, 0, ACTS.swap);
  const swap = inOut(span(s, 0.1, 0.65));
  if (s > 0) {
    wave = { from: [...order], to: swapped(order), mix: smooth(span(s, 0.3, 0.75)) };
    if (s >= 0.15) caption = BLOCKS;
  }

  return {
    header,
    wire,
    chrome: smooth(span(draw, 0.15, 0.8)),
    tiles,
    swap,
    wave,
    caption,
    time: 0.07 * u,
  };
}

export function finalScene(order: readonly Stage[]): Scene {
  return {
    header: 1,
    wire: BLOCKS + 1,
    chrome: 1,
    tiles: order.map(() => ({ drop: 0, opacity: 1 })),
    swap: 0,
    wave: { from: [...order], to: [...order], mix: 1 },
    caption: -1,
    time: 0,
  };
}
