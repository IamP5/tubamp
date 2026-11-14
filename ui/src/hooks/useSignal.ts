/**
 * The input level's recent past — what the player just did — for displays that
 * show an effect acting on it: a delay's repeats lighting up one delay time
 * apart, a reverb's tail holding for its decay.
 *
 * One module-level engine, like useMeters: it subscribes to the bridge's 30 Hz
 * `meters` event and, on the shared rAF driver, follows the input with an
 * instant attack and a short release (so a pick attack is a crisp edge, not a
 * smear), sampling the result into a ring at a fixed rate. Never React state.
 *
 * Levels are 0..1 over a −48…0 dB window: 0 is silence as far as a picture is
 * concerned, 1 is full scale.
 *
 * This is the plugin's INPUT, not the level at a block. A display that uses it
 * is drawing "what you played, through this effect's own settings" — honest
 * about the timing, approximate about the amount.
 */
import { useEffect } from "react";
import { bridge } from "../bridge";
import { onFrame } from "./rafDriver";

const FLOOR_DB = -48;
/** Samples per second kept in the ring, and how far back it reaches. */
const RATE = 60;
const SECONDS = 8;
const SIZE = RATE * SECONDS;
/** Release of the follower, seconds: short enough to separate two notes. */
const RELEASE_TAU = 0.09;

const ring = new Float32Array(SIZE);
let head = 0;
let target = 0;
let current = 0;
let carry = 0;

function toLevel(linear: number): number {
  if (linear <= 1e-6) return 0;
  const db = 20 * Math.log10(linear);
  return Math.min(1, Math.max(0, (db - FLOOR_DB) / -FLOOR_DB));
}

export interface Signal {
  /** The input level right now, 0..1. */
  now(): number;
  /** The input level `seconds` ago, 0..1; 0 beyond the ring's reach. */
  at(seconds: number): number;
}

const signal: Signal = {
  now: () => current,
  at(seconds) {
    if (!(seconds > 0)) return current;
    const back = Math.round(seconds * RATE);
    if (back >= SIZE) return 0;
    return ring[(head - back + SIZE * 2) % SIZE] ?? 0;
  },
};

let refCount = 0;
let stopBridge: (() => void) | null = null;
let stopFrames: (() => void) | null = null;

function start(): void {
  stopBridge = bridge.on("meters", ({ in: input }) => {
    target = toLevel(input);
  });
  stopFrames = onFrame((dt) => {
    current = target >= current ? target : target + (current - target) * Math.exp(-dt / RELEASE_TAU);
    if (current < 1e-3) current = 0;
    // Fixed-rate sampling, whatever the display's refresh rate is.
    carry += dt * RATE;
    while (carry >= 1) {
      carry -= 1;
      head = (head + 1) % SIZE;
      ring[head] = current;
    }
  });
}

function stop(): void {
  stopBridge?.();
  stopFrames?.();
  stopBridge = null;
  stopFrames = null;
  ring.fill(0);
  head = 0;
  target = 0;
  current = 0;
  carry = 0;
}

/** The shared input-level history; the engine runs while at least one
 *  component uses it. */
export function useSignal(): Signal {
  useEffect(() => {
    refCount += 1;
    if (refCount === 1) start();
    return () => {
      refCount -= 1;
      if (refCount === 0) stop();
    };
  }, []);

  return signal;
}
