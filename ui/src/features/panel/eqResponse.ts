/**
 * The EQ's response laws (docs/FX-ENGINES.md §4.4), pure (no React): the
 * display draws them and the mock bridge applies them to its fake spectrum, so
 * the dev preview's post spectrum answers the same knobs the curve does.
 *
 *   Classic     bass shelf 150 Hz (4 dB/unit), mid bell 425 Hz (3 dB/unit,
 *               Q 1.5 cutting / 0.7 boosting), treble shelf 1800 Hz (2 dB/unit)
 *   Parametric  low cut, the same shelves, a mid bell at Mid freq / Mid Q,
 *               tilt (first order, pivot 700 Hz), high cut, level
 *   Stack       the Yeh–Smith passive tone stack with the voice's parts and
 *               make-up, then level
 *   Graphic     seven octave bells, Q √2, then level
 */
import { bellDb, highCutDb, highShelfDb, lowCutDb, lowShelfDb } from "./filterDb";

export const CLASSIC = 0;
export const PARAMETRIC = 1;
export const STACK = 2;
export const GRAPHIC = 3;

/** Band id suffix, centre frequency and its tick, low to high. */
export const GRAPHIC_BANDS = [
  ["g100", 100, "100"],
  ["g200", 200, "200"],
  ["g400", 400, "400"],
  ["g800", 800, "800"],
  ["g1k6", 1600, "1.6k"],
  ["g3k2", 3200, "3.2k"],
  ["g6k4", 6400, "6.4k"],
] as const;

/* ─────────────────────────────── responses ─────────────────────────────── */

/** dB per knob unit away from 5, and the band frequency (Classic, Parametric). */
export const BASS = { db: 4, hz: 150 };
export const MID = { db: 3, hz: 425 };
export const TREBLE = { db: 2, hz: 1800 };

/** First-order tilt about 700 Hz: −tilt dB at DC, +tilt dB at the top. */
export function tiltDb(f: number, tilt: number): number {
  const t = Math.pow(10, tilt / 20);
  const s = f / 700;
  return 10 * Math.log10((t * t * s * s + 1) / (s * s + t * t));
}

/** Stack parts per voice (Tweed, Blackface, British, Top Boost):
 *  R1 treble, R2 bass, R3 mid, R4 slope, C1, C2, C3, make-up dB. */
export const VOICES: readonly (readonly number[])[] = [
  [250e3, 1e6, 25e3, 56e3, 250e-12, 20e-9, 20e-9, 8.9],
  [250e3, 250e3, 10e3, 100e3, 250e-12, 100e-9, 47e-9, 15.1],
  [220e3, 1e6, 22e3, 33e3, 470e-12, 22e-9, 22e-9, 6.4],
  [1e6, 1e6, 10e3, 100e3, 50e-12, 22e-9, 22e-9, 13.6],
];

/** The stack's audio-taper pot law through (0.5, 0.3). */
const STACK_B = (1 / 0.3 - 1) ** 2;
const stackTaper = (x: number): number => (Math.pow(STACK_B, x) - 1) / (STACK_B - 1);

/** Yeh & Smith (DAFx-06) closed form, transcribed from the study's design.py
 *  `yeh_coeffs`; knobs 0..10 as the engine reads them. Returns dB at f. */
export function stackResponse(voice: number, bass: number, mid: number, treble: number): (f: number) => number {
  const [R1, R2, R3, R4, C1, C2, C3, makeup] = (VOICES[voice] ?? VOICES[2]!) as [
    number, number, number, number, number, number, number, number,
  ];
  const t = treble / 10;
  const m = stackTaper(mid / 10);
  const l = stackTaper(bass / 10);
  const b1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
  const b2 =
    t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) -
    m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
    m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
    l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) +
    l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) +
    (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
  const c123 = C1 * C2 * C3;
  const b3 =
    l * m * (c123 * R1 * R2 * R3 + c123 * R2 * R3 * R4) -
    m * m * (c123 * R1 * R3 * R3 + c123 * R3 * R3 * R4) +
    m * (c123 * R1 * R3 * R3 + c123 * R3 * R3 * R4) +
    t * c123 * R1 * R3 * R4 -
    t * m * c123 * R1 * R3 * R4 +
    t * l * c123 * R1 * R2 * R4;
  const a1 = C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4 + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
  const a2 =
    m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
    l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) -
    m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
    l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4) +
    (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 + C2 * C3 * R3 * R4);
  const a3 =
    l * m * (c123 * R1 * R2 * R3 + c123 * R2 * R3 * R4) -
    m * m * (c123 * R1 * R3 * R3 + c123 * R3 * R3 * R4) +
    m * (c123 * R3 * R3 * R4 + c123 * R1 * R3 * R3 - c123 * R1 * R3 * R4) +
    l * c123 * R1 * R2 * R4 +
    c123 * R1 * R3 * R4;
  return (f) => {
    const w = 2 * Math.PI * f;
    const w2 = w * w;
    /* s = jw: numerator b1 s + b2 s² + b3 s³, denominator 1 + a1 s + a2 s² + a3 s³. */
    const nr = -b2 * w2;
    const ni = b1 * w - b3 * w2 * w;
    const dr = 1 - a2 * w2;
    const di = a1 * w - a3 * w2 * w;
    return 10 * Math.log10((nr * nr + ni * ni) / (dr * dr + di * di)) + makeup;
  };
}

/** What the response reads, in the parameters' scaled units. */
export interface EqValues {
  bass: number;
  mid: number;
  treble: number;
  lowCut: number;
  highCut: number;
  midFreq: number;
  midQ: number;
  tilt: number;
  level: number;
  /** The seven graphic bands, dB, low to high. */
  bands: readonly number[];
}

/** dB away from a knob's centre (5) at `perUnit` dB per unit. */
export const knobDb = (value: number, perUnit: number): number => perUnit * (value - 5);

export function classicResponse(v: EqValues): (f: number) => number {
  const mid = knobDb(v.mid, MID.db);
  return (f) =>
    lowShelfDb(f, BASS.hz, knobDb(v.bass, BASS.db)) +
    bellDb(f, MID.hz, mid, mid < 0 ? 1.5 : 0.7) +
    highShelfDb(f, TREBLE.hz, knobDb(v.treble, TREBLE.db));
}

export function parametricResponse(v: EqValues): (f: number) => number {
  const lowOn = v.lowCut > 20.5; /* the engine skips a cut parked at its end */
  const highOn = v.highCut < 19999;
  return (f) =>
    (lowOn ? lowCutDb(f, v.lowCut) : 0) +
    lowShelfDb(f, BASS.hz, knobDb(v.bass, BASS.db)) +
    bellDb(f, v.midFreq, knobDb(v.mid, MID.db), v.midQ) +
    highShelfDb(f, TREBLE.hz, knobDb(v.treble, TREBLE.db)) +
    tiltDb(f, v.tilt) +
    (highOn ? highCutDb(f, v.highCut) : 0) +
    v.level;
}

export function graphicResponse(v: EqValues): (f: number) => number {
  return (f) => v.bands.reduce((sum, g, i) => sum + bellDb(f, GRAPHIC_BANDS[i]?.[1] ?? 1000, g, Math.SQRT2), v.level);
}

/** The whole response of one type (and voice, for the stack) at rest. */
export function eqResponse(type: number, voice: number, v: EqValues): (f: number) => number {
  if (type === PARAMETRIC) return parametricResponse(v);
  if (type === STACK) {
    const stack = stackResponse(voice, v.bass, v.mid, v.treble);
    return (f) => stack(f) + v.level;
  }
  if (type === GRAPHIC) return graphicResponse(v);
  return classicResponse(v);
}
