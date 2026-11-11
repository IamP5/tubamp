/**
 * Gate and compressor meters as motion values — never React state.
 *
 * Modelled on useMeters: one module-level engine subscribes to the bridge's
 * 30 Hz `dynMeters` event (docs/FX-ENGINES.md §2.8) and the shared rAF driver
 * eases the displayed values. Ballistics are the spec's: instant rise, then a
 * fall of 20 dB per second (`DYN_FALL_DB_PER_SECOND`). Everything stays in dB —
 * the meters normalise over their own scale with `useTransform`.
 *
 * `gateState` is not smoothed (it is a lamp): -1 inactive, 0 closed, 1 hold,
 * 2 open.
 */
import { useEffect } from "react";
import { motionValue, type MotionValue } from "motion/react";
import { bridge } from "../bridge";
import { onFrame } from "./rafDriver";

/** Fall rate of every dynamics meter, dB per second. */
export const DYN_FALL_DB_PER_SECOND = 20;
/** The gate's two channels fall faster: its state and gain reduction change in
 *  milliseconds, and at 20 dB/s the bars would still show a closed gate for
 *  seconds after the lamp says open. */
export const GATE_FALL_DB_PER_SECOND = 240;
/** Gate gain reduction can read 90 dB (120 in Classic); the meter shows 0..60. */
export const GATE_GR_SCALE_DB = 60;
/** Compressor gain reduction meter scale, 0..20 dB. */
export const COMP_GR_SCALE_DB = 20;
/** Gate detector level window, dBFS. */
export const GATE_LEVEL_MIN_DB = -90;
export const GATE_LEVEL_MAX_DB = 0;
const LEVEL_FLOOR_DB = -120;

export interface DynMeters {
  /** -1 inactive, 0 closed, 1 hold (closing), 2 open. */
  gateState: MotionValue<number>;
  /** Smoothed gate gain reduction, dB >= 0. */
  gateGr: MotionValue<number>;
  /** Smoothed gate detector level, dBFS. */
  gateLevel: MotionValue<number>;
  /** Smoothed compressor gain reduction per instance (0 = comp, 1 = comp2, 2 = comp3), dB. */
  compGr: readonly [MotionValue<number>, MotionValue<number>, MotionValue<number>];
}

interface Channel {
  value: MotionValue<number>;
  target: number;
  current: number;
  fall: number;
  /** What the channel rests at: 0 dB of reduction, -120 dBFS of level. */
  rest: number;
}

function makeChannel(rest: number, fall = DYN_FALL_DB_PER_SECOND): Channel {
  return { value: motionValue(rest), target: rest, current: rest, rest, fall };
}

const gateGr = makeChannel(0, GATE_FALL_DB_PER_SECOND);
const gateLevel = makeChannel(LEVEL_FLOOR_DB, GATE_FALL_DB_PER_SECOND);
const compGr = [makeChannel(0), makeChannel(0), makeChannel(0)] as const;
const gateState = motionValue(-1);

const dynMeters: DynMeters = {
  gateState,
  gateGr: gateGr.value,
  gateLevel: gateLevel.value,
  compGr: [compGr[0].value, compGr[1].value, compGr[2].value],
};

/** Instant rise, linear fall in dB at the channel's own rate. */
function advance(ch: Channel, dt: number): void {
  if (ch.target >= ch.current) {
    ch.current = ch.target;
  } else {
    ch.current = Math.max(ch.target, ch.current - ch.fall * dt);
  }
  ch.value.set(ch.current);
}

function reset(ch: Channel): void {
  ch.target = ch.rest;
  ch.current = ch.rest;
  ch.value.set(ch.rest);
}

let refCount = 0;
let stopBridge: (() => void) | null = null;
let stopFrames: (() => void) | null = null;

function start(): void {
  stopBridge = bridge.on("dynMeters", ({ gate, comp }) => {
    gateState.set(gate.state);
    gateGr.target = Math.max(0, gate.gr);
    gateLevel.target = Math.max(LEVEL_FLOOR_DB, gate.level);
    comp.forEach((gr, i) => {
      const ch = compGr[i];
      if (ch) ch.target = Math.max(0, gr);
    });
  });
  stopFrames = onFrame((dt) => {
    advance(gateGr, dt);
    advance(gateLevel, dt);
    for (const ch of compGr) advance(ch, dt);
  });
}

function stop(): void {
  stopBridge?.();
  stopFrames?.();
  stopBridge = null;
  stopFrames = null;
  gateState.set(-1);
  for (const ch of [gateGr, gateLevel, ...compGr]) reset(ch);
}

/** The shared dynamics-meter motion values; the engine runs while at least one
 *  component uses them. */
export function useDynMeters(): DynMeters {
  useEffect(() => {
    refCount += 1;
    if (refCount === 1) start();
    return () => {
      refCount -= 1;
      if (refCount === 0) stop();
    };
  }, []);

  return dynMeters;
}
