/**
 * Not audio: a picture drawn from a few honest rules (a gate zeroes the quiet, a drive
 * clips, a cab low-passes) so the waveform changes the way the sound would. Pure and
 * deterministic, so the page can redraw it from scroll position alone.
 */

export type Stage = "gate" | "comp" | "drive" | "amp" | "cab" | "eq" | "mod" | "delay" | "reverb";

/** Samples per unit of signal time. One unit holds four plucked notes. */
export const RATE = 720;
export const DRIVE_CLIP = 0.5;

const NOISE = 0.07;
const LOOKBACK = RATE;

type Note = { at: number; amp: number; cycles: number };

/** Uneven picking on purpose: the second note is soft, so the compressor has work to do. */
const NOTES: readonly Note[] = [
  { at: 0.02, amp: 1, cycles: 80 },
  { at: 0.27, amp: 0.32, cycles: 96 },
  { at: 0.52, amp: 0.85, cycles: 72 },
  { at: 0.77, amp: 0.55, cycles: 88 },
];

const TAU = Math.PI * 2;
const ATTACK = 0.004;
const DECAY = 0.04;

/** 0..1 loudness of the plucked notes at time u (the signal repeats every unit). */
export function noteEnvelope(u: number): number {
  let e = 0;
  for (const n of NOTES) {
    const t = (((u - n.at) % 1) + 1) % 1;
    e += n.amp * (1 - Math.exp(-t / ATTACK)) * Math.exp(-t / DECAY);
  }
  return e;
}

function hash(n: number): number {
  let h = Math.imul(n ^ 0x9e3779b9, 0x85ebca6b);
  h = Math.imul(h ^ (h >>> 13), 0xc2b2ae35);
  h ^= h >>> 16;
  return (h >>> 0) / 0xffffffff;
}

function drySample(n: number): number {
  const u = n / RATE;
  let s = 0;
  for (const note of NOTES) {
    const t = (((u - note.at) % 1) + 1) % 1;
    const env = note.amp * (1 - Math.exp(-t / ATTACK)) * Math.exp(-t / DECAY);
    const ph = TAU * note.cycles * t;
    s += env * 0.72 * (Math.sin(ph) + 0.3 * Math.sin(2 * ph + 0.6) + 0.12 * Math.sin(3 * ph + 1.1));
  }
  const k = ((n % RATE) + RATE) % RATE;
  return s + NOISE * (hash(k) * 2 - 1);
}

export type Span = {
  /** First sample index shown. Whole samples, so the noise never shimmers as it scrolls. */
  start: number;
  length: number;
};

function dryBuffer(from: number, length: number): Float32Array {
  const out = new Float32Array(length);
  for (let i = 0; i < length; i++) out[i] = drySample(from + i);
  return out;
}

export function drySignal(win: Span): Float32Array {
  return dryBuffer(win.start, win.length);
}

type Process = (x: Float32Array, from: number) => Float32Array;

function lowpass(x: Float32Array, a: number): Float32Array {
  const y = new Float32Array(x.length);
  let s = x[0];
  for (let i = 0; i < x.length; i++) {
    s += a * (x[i] - s);
    y[i] = s;
  }
  return y;
}

const gate: Process = (x) => {
  const y = new Float32Array(x.length);
  let env = 0;
  let open = false;
  let g = 0;
  for (let i = 0; i < x.length; i++) {
    env = Math.max(Math.abs(x[i]), env * 0.9);
    if (open && env < 0.1) open = false;
    else if (!open && env > 0.16) open = true;
    g = open ? Math.min(1, g + 0.25) : Math.max(0, g - 0.12);
    y[i] = x[i] * g;
  }
  return y;
};

const comp: Process = (x) => {
  const y = new Float32Array(x.length);
  const threshold = 0.1;
  const ratio = 10;
  const makeup = 3.6;
  let env = 0;
  for (let i = 0; i < x.length; i++) {
    env = Math.max(Math.abs(x[i]), env * 0.993);
    const gain = env > threshold ? (threshold + (env - threshold) / ratio) / env : 1;
    y[i] = x[i] * gain * makeup;
  }
  return y;
};

const drive: Process = (x) => {
  const y = new Float32Array(x.length);
  const gain = 4;
  for (let i = 0; i < x.length; i++) {
    const v = (gain * x[i]) / DRIVE_CLIP;
    y[i] = (DRIVE_CLIP * v) / Math.pow(1 + v ** 4, 0.25);
  }
  return y;
};

const amp: Process = (x) => {
  const y = new Float32Array(x.length);
  for (let i = 0; i < x.length; i++) {
    const v = 2.6 * x[i];
    y[i] = 0.78 * Math.tanh(v + 0.25 * v * v);
  }
  const body = lowpass(y, 0.18);
  for (let i = 0; i < y.length; i++) y[i] = y[i] + 0.5 * body[i];
  return y;
};

const cab: Process = (x) => {
  const lp = lowpass(lowpass(lowpass(x, 0.5), 0.5), 0.6);
  const dc = lowpass(lp, 0.01);
  const y = new Float32Array(x.length);
  for (let i = 0; i < x.length; i++) y[i] = 1.25 * (lp[i] - dc[i]);
  return y;
};

const eq: Process = (x) => {
  const lp = lowpass(x, 0.75);
  const y = new Float32Array(x.length);
  for (let i = 0; i < x.length; i++) y[i] = 0.75 * lp[i] + 2.4 * (x[i] - lp[i]);
  return y;
};

const mod: Process = (x, from) => {
  const y = new Float32Array(x.length);
  for (let i = 0; i < x.length; i++) {
    const u = (from + i) / RATE;
    const lfo = 0.5 + 0.5 * Math.sin(TAU * 10 * u);
    const d = 5 + 4 * Math.sin(TAU * 3 * u);
    const j = i - Math.round(d);
    const wet = j >= 0 ? x[j] : 0;
    y[i] = (x[i] * 0.8 + wet * 0.35) * (1 - 0.5 * lfo);
  }
  return y;
};

const delay: Process = (x) => {
  const y = new Float32Array(x.length);
  const w = new Float32Array(x.length);
  const d = Math.round(0.125 * RATE);
  for (let i = 0; i < x.length; i++) {
    w[i] = x[i] + (i >= d ? 0.4 * w[i - d] : 0);
    y[i] = x[i] + (i >= d ? 0.55 * w[i - d] : 0);
  }
  return y;
};

/** A diffuse, decaying impulse response: the gaps fill with a wash, not with copies. */
const IMPULSE = (() => {
  const length = Math.round(0.4 * RATE);
  const h = new Float32Array(length);
  for (let k = 1; k < length; k++) h[k] = (hash(k * 7919) * 2 - 1) * Math.exp(-k / (0.12 * RATE));
  return lowpass(h, 0.35);
})();

const reverb: Process = (x) => {
  const wet = new Float32Array(x.length);
  for (let i = 0; i < x.length; i++) {
    const v = x[i];
    if (v === 0) continue;
    const end = Math.min(x.length - i, IMPULSE.length);
    for (let k = 1; k < end; k++) wet[i + k] += v * IMPULSE[k];
  }
  const y = new Float32Array(x.length);
  for (let i = 0; i < x.length; i++) y[i] = 0.7 * x[i] + 0.26 * wet[i];
  return y;
};

const STAGES: Record<Stage, Process> = { gate, comp, drive, amp, cab, eq, mod, delay, reverb };

export const isStage = (id: string): id is Stage => Object.hasOwn(STAGES, id);

/**
 * Blocks with memory (compressor, delay, reverb) are run over a unit of signal
 * before the window so their state has settled.
 */
export function renderSignal(chain: readonly Stage[], win: Span): Float32Array {
  if (chain.length === 0) return drySignal(win);
  const from = win.start - LOOKBACK;
  let x = dryBuffer(from, LOOKBACK + win.length);
  for (const stage of chain) x = STAGES[stage](x, from);
  return x.slice(LOOKBACK);
}
