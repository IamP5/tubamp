/**
 * The EQ analyser as per-frame data — never React state.
 *
 * Modelled on useDynMeters: one module-level engine. While at least one
 * component watches an instance it tells the engine (`bridge.eqWatch`), takes
 * the ~30 Hz `eqSpectrum` event in, and runs meter ballistics on the shared
 * frame loop: a point rises instantly and falls linearly in dB at the rate the
 * analyser's Speed picks (`EQ_SPECTRUM_FALL_DB_PER_SECOND`), down to
 * `EQ_SPECTRUM_FLOOR_DB`. Freeze stops the fall, so every point holds the
 * maximum it has seen (still rising instantly); released, the points fall from
 * where they are. Resolution is the engine's business: the display only sees
 * the same 256 points, sharper or blunter. Watchers
 * are ref-counted per instance; the instance acquired last wins, and when no
 * one watches the engine is told -1.
 *
 * The values are raw dBFS per analyser point (no tilt, no smoothing across
 * frequency): presentation is the display's business.
 */
import { useCallback, useEffect, useRef } from "react";
import { bridge, type EqAnalyserStep, type EqInstance } from "../bridge";
import { onFrame } from "./rafDriver";

/** Points per spectrum, as the engine sends them. */
export const EQ_SPECTRUM_POINTS = 256;
/** Release of every displayed point per Speed step (Slow, Medium, Fast), dB per
 *  second; attack is always instant. Medium is the rate of every other meter. */
export const EQ_SPECTRUM_FALL_DB_PER_SECOND: Readonly<Record<EqAnalyserStep, number>> = {
  0: 10,
  1: 20,
  2: 40,
};
/** Where a point rests with no signal, dBFS. */
export const EQ_SPECTRUM_FLOOR_DB = -120;

/** Frequency of analyser point `i`, Hz (20 Hz .. 20 kHz on a log axis). */
export const eqSpectrumHz = (i: number): number => 20 * Math.pow(1000, i / (EQ_SPECTRUM_POINTS - 1));

/** The displayed spectrum, dBFS per point. Read it inside the paint callback;
 *  the arrays are reused frame to frame. */
export interface EqSpectrumFrame {
  /** What enters the EQ block. */
  pre: Float32Array;
  /** What leaves it. */
  post: Float32Array;
}

/** Called once on subscribe and then on every frame the values move; `null`
 *  when there is nothing to show (nothing watched, or everything at the floor). */
export type EqSpectrumPaint = (frame: EqSpectrumFrame | null) => void;

interface Pair {
  target: Float32Array;
  shown: Float32Array;
}

const makePair = (): Pair => ({
  target: new Float32Array(EQ_SPECTRUM_POINTS).fill(EQ_SPECTRUM_FLOOR_DB),
  shown: new Float32Array(EQ_SPECTRUM_POINTS).fill(EQ_SPECTRUM_FLOOR_DB),
});

const pre = makePair();
const post = makePair();
const frame: EqSpectrumFrame = { pre: pre.shown, post: post.shown };

/** Watchers per instance; the order instances were acquired in (last wins). */
const counts = new Map<EqInstance, number>();
const order: EqInstance[] = [];
/** Each watcher's repaint (it knows its own instance, see `shownFor`). */
const painters = new Set<() => void>();
let watched: EqInstance | -1 = -1;
/** The fall rate in force (dB/s), 0 while frozen; see `useEqSpectrum`. */
let fallRate = EQ_SPECTRUM_FALL_DB_PER_SECOND[1];
let resting = true;
let stopBridge: (() => void) | null = null;
let stopFrames: (() => void) | null = null;

function clear(): void {
  for (const pair of [pre, post]) {
    pair.target.fill(EQ_SPECTRUM_FLOOR_DB);
    pair.shown.fill(EQ_SPECTRUM_FLOOR_DB);
  }
  resting = true;
}

function paintAll(): void {
  for (const repaint of painters) repaint();
}

function take(into: Float32Array, codes: readonly number[]): void {
  for (let i = 0; i < EQ_SPECTRUM_POINTS; i += 1) {
    const v = codes[i] ?? 0;
    /* 0 is "nothing there" (silence), the floor rather than a level. */
    into[i] = v > 0 ? Math.min(0, v / 2 - 120) : EQ_SPECTRUM_FLOOR_DB;
  }
}

/** Instant rise, linear fall; true if anything moved. */
function advance(pair: Pair, fall: number): boolean {
  let moved = false;
  const { target, shown } = pair;
  for (let i = 0; i < EQ_SPECTRUM_POINTS; i += 1) {
    const t = target[i]!;
    const s = shown[i]!;
    const next = t >= s ? t : Math.max(t, s - fall);
    if (next !== s) {
      shown[i] = next;
      moved = true;
    }
  }
  return moved;
}

function atFloor(pair: Pair): boolean {
  for (let i = 0; i < EQ_SPECTRUM_POINTS; i += 1) {
    if (pair.shown[i]! > EQ_SPECTRUM_FLOOR_DB || pair.target[i]! > EQ_SPECTRUM_FLOOR_DB) return false;
  }
  return true;
}

function start(): void {
  stopBridge = bridge.on("eqSpectrum", (msg) => {
    if (msg.instance !== watched) return;
    take(pre.target, msg.pre);
    take(post.target, msg.post);
  });
  stopFrames = onFrame((dt) => {
    const fall = fallRate * dt;
    const movedPre = advance(pre, fall);
    const movedPost = advance(post, fall);
    if (!movedPre && !movedPost) return;
    resting = atFloor(pre) && atFloor(post);
    paintAll();
  });
}

function stop(): void {
  stopBridge?.();
  stopFrames?.();
  stopBridge = null;
  stopFrames = null;
}

/** Point the engine at the instance acquired last that is still watched. */
function rewatch(): void {
  const next = order.length > 0 ? order[order.length - 1]! : -1;
  if (next === watched) return;
  watched = next;
  clear();
  if (next === -1) stop();
  else if (!stopBridge) start();
  void bridge.eqWatch(next);
  paintAll();
}

function acquire(instance: EqInstance): void {
  counts.set(instance, (counts.get(instance) ?? 0) + 1);
  const at = order.indexOf(instance);
  if (at >= 0) order.splice(at, 1);
  order.push(instance);
  rewatch();
}

function release(instance: EqInstance): void {
  const left = (counts.get(instance) ?? 1) - 1;
  if (left > 0) {
    counts.set(instance, left);
    return;
  }
  counts.delete(instance);
  const at = order.indexOf(instance);
  if (at >= 0) order.splice(at, 1);
  rewatch();
}

/** What a watcher of `instance` sees right now. */
const shownFor = (instance: EqInstance | null): EqSpectrumFrame | null =>
  instance !== null && instance === watched && !resting ? frame : null;

/** How the displayed points move: the analyser's Speed, and Freeze. */
export interface EqSpectrumBallistics {
  speed: EqAnalyserStep;
  freeze: boolean;
  /** The engine's FFT size choice. The display does not use the number: a
   *  change clears what is shown, held maxima included, so the picture
   *  rebuilds from the floor at the new resolution rather than mixing the two
   *  (what Logic's Channel EQ does; docs/research/eq-analyser.md). */
  resolution: EqAnalyserStep;
}

/** The resolution the shown points were measured at; see `resolution` above. */
let shownResolution: EqAnalyserStep | null = null;

/**
 * Watches EQ `instance` (0 = eq, 1 = eq2, 2 = eq3; `null` watches nothing) and
 * calls `paint` with the displayed spectrum: once now, then on every frame it
 * moves. `paint` writes to the DOM through refs; it must not set React state.
 * `ballistics` sets the fall for the one shared engine (the preferences are
 * shared by every EQ, so whichever watcher set them last is right). Returns
 * `repaint`, for when the picture changes under the same data (a resize).
 */
export function useEqSpectrum(
  instance: EqInstance | null,
  paint: EqSpectrumPaint,
  { speed, freeze, resolution }: EqSpectrumBallistics,
): () => void {
  const latest = useRef(paint);
  const mine = useRef(instance);
  useEffect(() => {
    latest.current = paint;
    mine.current = instance;
  });

  useEffect(() => {
    fallRate = freeze ? 0 : EQ_SPECTRUM_FALL_DB_PER_SECOND[speed];
  }, [speed, freeze]);

  useEffect(() => {
    if (shownResolution !== null && shownResolution !== resolution) {
      clear();
      paintAll();
    }
    shownResolution = resolution;
  }, [resolution]);

  useEffect(() => {
    const painter = (): void => latest.current(shownFor(mine.current));
    painters.add(painter);
    return () => {
      painters.delete(painter);
    };
  }, []);

  useEffect(() => {
    mine.current = instance;
    if (instance !== null) acquire(instance);
    latest.current(shownFor(instance));
    return () => {
      if (instance !== null) release(instance);
    };
  }, [instance]);

  return useCallback(() => latest.current(shownFor(mine.current)), []);
}
