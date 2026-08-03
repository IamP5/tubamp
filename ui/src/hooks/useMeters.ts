/**
 * Meter levels as motion values — never React state.
 *
 * One module-level engine subscribes to the bridge's 30 Hz `meters` event, and
 * the shared rAF driver smooths towards the latest peak with asymmetric
 * attack/release time constants (motion-design.md §2.12). Components read the
 * returned MotionValues directly (`style={{ scaleY: level }}`), so a meter never
 * re-renders React.
 *
 * All values are normalised 0..1 over the meter's dB window (−60…+6 dB), i.e.
 * already display-space — see `dbToMeterPosition`.
 */
import { useEffect } from "react";
import { motionValue, type MotionValue } from "motion/react";
import { bridge } from "../bridge";
import { meterSmoothing, smoothingCoef } from "../theme/motion";
import { onFrame } from "./rafDriver";

export const METER_MIN_DB = -60;
export const METER_MAX_DB = 6;
/** Segment colour breakpoints (PeakMeter parity). */
export const METER_MID_DB = -12;
export const METER_HIGH_DB = -3;
export const METER_SEGMENTS = 10;

export function gainToDb(linear: number): number {
  return linear > 1e-6 ? 20 * Math.log10(linear) : METER_MIN_DB;
}

/** Linear peak → 0..1 position on the meter ladder. */
export function linearToMeterPosition(linear: number): number {
  return dbToMeterPosition(gainToDb(linear));
}

export function dbToMeterPosition(db: number): number {
  const p = (db - METER_MIN_DB) / (METER_MAX_DB - METER_MIN_DB);
  return Math.min(1, Math.max(0, p));
}

export interface MeterChannel {
  /** Smoothed level, 0..1 of the meter ladder. */
  level: MotionValue<number>;
  /** Peak-hold tick position, 0..1; 0 when nothing is held. */
  hold: MotionValue<number>;
}

export interface Meters {
  input: MeterChannel;
  output: MeterChannel;
}

interface ChannelEngine extends MeterChannel {
  target: number;
  current: number;
  holdValue: number;
  holdSinceMs: number;
}

function makeChannel(): ChannelEngine {
  return {
    level: motionValue(0),
    hold: motionValue(0),
    target: 0,
    current: 0,
    holdValue: 0,
    holdSinceMs: 0,
  };
}

const channels: { input: ChannelEngine; output: ChannelEngine } = {
  input: makeChannel(),
  output: makeChannel(),
};

const meters: Meters = {
  input: { level: channels.input.level, hold: channels.input.hold },
  output: { level: channels.output.level, hold: channels.output.hold },
};

function advance(ch: ChannelEngine, dt: number, now: number): void {
  const rising = ch.target > ch.current;
  const coef = smoothingCoef(
    dt,
    rising ? meterSmoothing.attackTau : meterSmoothing.releaseTau,
  );
  ch.current += (ch.target - ch.current) * coef;
  if (ch.current < 1e-4) ch.current = 0;
  ch.level.set(ch.current);

  // Peak-hold: snaps up with the level, dwells, then falls at a steady rate.
  if (ch.current >= ch.holdValue) {
    ch.holdValue = ch.current;
    ch.holdSinceMs = now;
  } else if (now - ch.holdSinceMs > meterSmoothing.holdMs) {
    ch.holdValue = Math.max(
      ch.current,
      ch.holdValue - meterSmoothing.holdFallPerSecond * dt,
    );
  }
  ch.hold.set(ch.holdValue);
}

let refCount = 0;
let stopBridge: (() => void) | null = null;
let stopFrames: (() => void) | null = null;

function start(): void {
  stopBridge = bridge.on("meters", ({ in: input, out: output }) => {
    channels.input.target = linearToMeterPosition(input);
    channels.output.target = linearToMeterPosition(output);
  });
  stopFrames = onFrame((dt, now) => {
    advance(channels.input, dt, now);
    advance(channels.output, dt, now);
  });
}

function stop(): void {
  stopBridge?.();
  stopFrames?.();
  stopBridge = null;
  stopFrames = null;
  for (const ch of [channels.input, channels.output]) {
    ch.target = 0;
    ch.current = 0;
    ch.holdValue = 0;
    ch.level.set(0);
    ch.hold.set(0);
  }
}

/**
 * Returns the shared meter motion values and keeps the engine running while at
 * least one component uses them.
 */
export function useMeters(): Meters {
  useEffect(() => {
    refCount += 1;
    if (refCount === 1) start();
    return () => {
      refCount -= 1;
      if (refCount === 0) stop();
    };
  }, []);

  return meters;
}
