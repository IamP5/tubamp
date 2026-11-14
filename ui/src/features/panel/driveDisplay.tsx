/**
 * The drive editor's display (docs/FX-ENGINES.md §4.3.5): the waveshaper's
 * transfer curve beside the tone control's frequency response, both drawn
 * from the type's own circuit constants (src/dsp/Drive.cpp).
 *
 * Transfer curve: output against input for a note peaking at ±0.15 V (a hot
 * pickup; the engine reads 1 V per full scale) through the type's real
 * clipper law at mid-band, normalised so the input's peak lands on the plot's
 * corner — the shape, not the level (Level is a knob). What is left out, and
 * so makes it a picture of the direction rather than a measurement: the
 * frequency dependence of each gain stage (the Screamer's 720 Hz pre-clip
 * high-pass, the Fuzz's 720 Hz feedback poles, the Distortion's op-amp
 * bandwidth), the coupling high-passes and oversampling.
 *
 * Tone response: the type's tone stage evaluated exactly (|H(j2πf)|) from the
 * same component values, normalised to 0 dB at its peak.
 *
 * Live: a point on the transfer curve at the player's input level (its peak,
 * against the plot's ±0.15 V), so playing harder climbs into the clipped
 * part of the curve. Hidden in silence.
 */
import { useRef } from "react";
import type { BlockId, SliderParamId } from "../../bridge";
import { useSliderParam } from "../../hooks";
import {
  F_MAX,
  F_MIN,
  FreqGrid,
  HRules,
  Plot,
  PlotHandle,
  areaPath,
  clamp,
  curve,
  dbY,
  freqX,
  highCutDb,
  linePath,
  lerp,
  xFreq,
  plot as p,
  useEase,
  useLive,
} from "./display";

/** `drive_type` order (frozen). */
const CLASSIC = 0;
const SCREAMER = 1;
const DISTORTION = 2;
const FUZZ = 3;
const BOOST = 4;
const TREBLE = 5;

/* ─────────────────────────────── circuit math ──────────────────────────── */

const TWO_PI = 2 * Math.PI;

/** Input peak the transfer plot's x = ±1 stands for, in volts. */
const V_IN = 0.15;

/** 1N4148/1N914 SPICE model (Drive.cpp): Is and n·V_T. */
const IS = 2.52e-9;
const VT_N = 1.752 * 0.02585;

/** Drive.cpp `audioTaper`: 10 % of the track at mid travel. */
const audioTaper = (x: number): number => (Math.pow(81, x) - 1) / 80;

/**
 * Antiparallel diode pair behind resistance R (k = 2·Is·R): the voltage v
 * across it for drive u, from u = v + k·sinh(v/a). Newton from the upper
 * bound, as Drive.cpp starts its Halley iteration.
 */
function diode(u: number, k: number): number {
  const mag = Math.abs(u);
  let v = Math.min(mag, VT_N * Math.asinh(mag / k));
  for (let i = 0; i < 24; i++) {
    const f = v + k * Math.sinh(v / VT_N) - mag;
    const df = 1 + (k / VT_N) * Math.cosh(v / VT_N);
    const next = v - f / df;
    if (Math.abs(next - v) < 1e-9) break;
    v = Math.max(0, next);
  }
  return Math.sign(u) * v;
}

/** The type's output for input `x` (−1..1 of the plot), before normalising. */
function shaper(type: number, gainDb: number): (x: number) => number {
  const d = clamp(gainDb / 36, 0, 1);
  const gain = Math.pow(10, gainDb / 20);
  switch (type) {
    case CLASSIC:
      /* tanh(x · gain) on the digital signal: the exact law. */
      return (x) => Math.tanh(gain * V_IN * x);
    case SCREAMER: {
      /* TS-808: diodes across R2 in the op-amp's feedback, the clean input
         summed back in (output = input + feedback-network voltage). */
      const r2 = 51e3 + 500e3 * audioTaper(d);
      const k = 2 * IS * r2;
      return (x) => V_IN * x + diode((r2 / 4.7e3) * V_IN * x, k);
    }
    case DISTORTION: {
      /* RAT: 0..62 dB at 1 kHz linear in the knob, the ±4.5 V rail, then
         1 kΩ into the diode pair to ground. No clean path. */
      const g = Math.pow(10, (62 * d) / 20);
      const k = 2 * IS * 1e3;
      return (x) => diode(clamp(g * V_IN * x, -4.5, 4.5), k);
    }
    case FUZZ: {
      /* Big Muff: booster (12.05, −1.0 dB of transistor loading), the
         Sustain divider −78 (1 − d)² dB, two inverting clipping stages
         (47, −2.5 dB of loading) with diodes across 470 kΩ, each with the
         bias's even-harmonic term v (1 + σ v / 2). */
      const booster = 12.05 * Math.pow(10, -1.0 / 20);
      const sustain = Math.pow(10, (-78 * (1 - d) * (1 - d)) / 20);
      const stage = 47 * Math.pow(10, -2.5 / 20);
      const k = 2 * IS * 470e3;
      const sigma = 0.0454;
      const clipStage = (u: number): number => {
        const v = -diode(stage * u, k);
        return v * (1 + (sigma * v) / 2);
      };
      return (x) => clipStage(clipStage(-booster * sustain * V_IN * x));
    }
    default:
      /* Boost and Treble are linear: a straight line whose slope is the gain
         (for Treble, the gain above its shelf). */
      return (x) => gain * x;
  }
}

/* Complex arithmetic for the tone stages, as [re, im]. */
type C = readonly [number, number];
const cadd = (a: C, b: C): C => [a[0] + b[0], a[1] + b[1]];
const cmul = (a: C, b: C): C => [a[0] * b[0] - a[1] * b[1], a[0] * b[1] + a[1] * b[0]];
const cdiv = (a: C, b: C): C => {
  const den = b[0] * b[0] + b[1] * b[1];
  return [(a[0] * b[0] + a[1] * b[1]) / den, (a[1] * b[0] - a[0] * b[1]) / den];
};
const cinv = (a: C): C => cdiv([1, 0], a);
const cabs = (a: C): number => Math.hypot(a[0], a[1]);
const real = (r: number): C => [r, 0];

/** The tone stage's magnitude in dB at `f` for the type, unnormalised. */
function toneStage(type: number, toneHz: number, gainDb: number): (f: number) => number {
  /* d and T as the engine reads them (§4.3.2). */
  const t = clamp(Math.log(toneHz / 500) / Math.log(24), 0, 1);
  switch (type) {
    case CLASSIC:
      /* 12 dB/oct state-variable low-pass at the knob. */
      return (f) => highCutDb(f, toneHz);
    case SCREAMER: {
      /* TS-808 tone/volume stage, §4.3.5's closed form. */
      const tp = clamp(t, 0.005, 0.995);
      const gl = 1 / (tp * 20e3);
      const gr = 1 / ((1 - tp) * 20e3);
      const g = gl + gr;
      const rs = 1e3;
      const cs = 0.22e-6;
      const gs = 1 / rs + 1 / 10e3;
      const c = 0.22e-6;
      const rz = 220;
      const rf = 1e3;
      const n0 = g / rs;
      const n1 = (c * (g * rz + 1 + rf * gr)) / rs;
      const d0 = gs * g;
      const d1 = gs * c * (g * rz + 1) + cs * g + gl * c;
      const d2 = cs * c * (g * rz + 1);
      return (f) => {
        const w = TWO_PI * f;
        const h = cdiv([n0, n1 * w], [d0 - d2 * w * w, d1 * w]);
        return 20 * Math.log10(cabs(h));
      };
    }
    case DISTORTION: {
      /* RAT Filter: one-pole low-pass, 1.5 kΩ + 100 kΩ A-taper into 3.3 nF. */
      const fc = 1 / (TWO_PI * (1.5e3 + 100e3 * audioTaper(1 - t)) * 3.3e-9);
      return (f) => -10 * Math.log10(1 + (f / fc) * (f / fc));
    }
    case FUZZ: {
      /* Ram's Head stack: 39 kΩ / 10 nF low-pass and 4 nF / 22 kΩ high-pass
         blended by a 100 kΩ linear pot, wiper loaded by 470 k ‖ 100 k. */
      const rt = 39e3;
      const ct = 10e-9;
      const cb = 4e-9;
      const rb = 22e3;
      const rm = 100e3;
      const rl = (470e3 * 100e3) / (570e3);
      const gma = 1 / (Math.max(t, 0.005) * rm);
      const gmb = 1 / (Math.max(1 - t, 0.005) * rm);
      return (f) => {
        const w = TWO_PI * f;
        const a1: C = [1 / rt + gma, w * ct];
        const b1: C = [1 / rb + gmb, w * cb];
        const sCb: C = [0, w * cb];
        /* VW (Gma + Gmb + 1/RL − Gma²/a1 − Gmb²/b1) = Gma/(RT a1) + Gmb sCB/b1 */
        const lhs = cadd(
          real(gma + gmb + 1 / rl),
          cadd(cmul(real(-gma * gma), cinv(a1)), cmul(real(-gmb * gmb), cinv(b1))),
        );
        const rhs = cadd(cmul(real(gma / rt), cinv(a1)), cmul(cmul(real(gmb), sCb), cinv(b1)));
        return 20 * Math.log10(cabs(cdiv(rhs, lhs)));
      };
    }
    case BOOST:
      /* One-pole high cut at the knob. */
      return (f) => -10 * Math.log10(1 + (f / toneHz) * (f / toneHz));
    case TREBLE: {
      /* First-order high shelf, geometric centre at the knob. */
      const root = Math.sqrt(Math.pow(10, gainDb / 20));
      const fz = toneHz / root;
      const fp = toneHz * root;
      return (f) => 10 * Math.log10((1 + (f / fz) ** 2) / (1 + (f / fp) ** 2));
    }
    default:
      return () => 0;
  }
}

/* ──────────────────────────────── the plots ────────────────────────────── */

/** Inset so the curve's ends and a handle at a plot edge are not cut. */
const PAD = 12;

/** x to frequency on the inset 20 Hz – 20 kHz axis, continued past its ends
 *  so a curve still runs to the plot's edges. */
export const padFreq = (x: number, iw: number, pad = PAD): number =>
  F_MIN * Math.pow(F_MAX / F_MIN, (x - pad) / iw);

/** The input level (0..1 of useSignal's 48 dB window) to the plot's x: its
 *  peak in volts (1 V per full scale, as the engine reads it) over V_IN. */
const levelX = (level: number): number => (level > 0 ? Math.pow(10, (48 * level - 48) / 20) / V_IN : 0);
/** Input level at which the point is fully shown. */
const DOT_FULL_LEVEL = 0.15;

export function TransferPlot({ block, type }: { block: BlockId; type: number }) {
  const gain = useSliderParam(`${block}_gain` as SliderParamId);
  /* Eased, so a type's starting point or a preset reshapes the curve over
     180 ms (the type itself, a different circuit, cuts). */
  const fn = shaper(type, useEase(gain.scaled));
  const linear = type === BOOST || type === TREBLE;
  /* Signed, so an inverting circuit (the Fuzz's three stages) still rises to
     the right; its asymmetry shows as the left end missing the corner. */
  const peak = linear ? 1 : fn(1) || 1;
  /* A straight line is drawn only where it is inside the frame. */
  const reach = linear ? Math.min(1, 1 / Math.max(fn(1), 1e-6)) : 1;

  const dot = useRef<SVGCircleElement>(null);
  useLive((signal) => {
    const el = dot.current;
    if (!el) return;
    const level = signal.now();
    const opacity = clamp(level / DOT_FULL_LEVEL, 0, 1);
    el.style.opacity = opacity.toFixed(3);
    if (opacity <= 0) return;
    const w = Number(el.dataset.w);
    const h = Number(el.dataset.h);
    const x = Math.min(reach, levelX(level));
    const out = linear ? fn(x) : clamp(fn(x) / peak, -1.05, 1.05);
    const px = lerp(x, -1, 1, PAD, w - PAD);
    const py = lerp(out, 1, -1, PAD, h - PAD);
    el.setAttribute("transform", `translate(${px.toFixed(1)} ${py.toFixed(1)})`);
  });

  return (
    <Plot flex={1} aria-label="Drive transfer curve">
      {({ w, h }) => {
        const px = (x: number): number => lerp(x, -1, 1, PAD, w - PAD);
        const py = (y: number): number => lerp(y, 1, -1, PAD, h - PAD);
        const cx = Math.round(px(0)) + 0.5;
        const cy = Math.round(py(0)) + 0.5;
        const points: [number, number][] = [];
        if (linear) {
          points.push([px(-reach), py(fn(-reach))], [px(reach), py(fn(reach))]);
        } else {
          const N = 160;
          for (let i = 0; i <= N; i++) {
            const x = -1 + (2 * i) / N;
            points.push([px(x), py(clamp(fn(x) / peak, -1.05, 1.05))]);
          }
        }
        return (
          <>
            <line className={p.axis} x1={PAD} x2={w - PAD} y1={cy} y2={cy} />
            <line className={p.axis} x1={cx} x2={cx} y1={PAD} y2={h - PAD} />
            <path className={p.ghost} d={linePath([[px(-1), py(-1)], [px(1), py(1)]])} />
            <text className={p.caption} x={w - PAD} y={cy + 16} textAnchor="end">
              In
            </text>
            {/* Out in the empty top-left quadrant: every shape here rises left to right. */}
            <text className={p.caption} x={cx - 6} y={PAD + 10} textAnchor="end">
              Out
            </text>
            <path className={p.live} d={linePath(points)} />
            <circle ref={dot} className={p.dot} r={3.5} data-w={w} data-h={h} />
          </>
        );
      }}
    </Plot>
  );
}

export function TonePlot({ block, type, label }: { block: BlockId; type: number; label: string }) {
  const tone = useSliderParam(`${block}_tone` as SliderParamId);
  const gain = useSliderParam(`${block}_gain` as SliderParamId);
  const toneHz = useEase(tone.scaled);
  const stage = toneStage(type, toneHz, useEase(gain.scaled));
  const TOP = 3;
  const BOTTOM = -30;

  return (
    <Plot flex={0.7} aria-label={`Drive ${label.toLowerCase()} response`}>
      {({ w, h }) => {
        const iw = w - 2 * PAD;
        const fx = (f: number): number => PAD + freqX(f, iw);
        const y = (db: number): number => dbY(Math.max(BOTTOM - 6, db), h, TOP, BOTTOM);
        /* Normalise to the peak over the audio band: the shape, not the level. */
        const raw = curve(w, (x) => stage(padFreq(x, iw)));
        const peak = raw.reduce((m, [, db]) => Math.max(m, db), -Infinity);
        const points = raw.map(([x, db]): [number, number] => [x, y(db - peak)]);
        return (
          <>
            <g transform={`translate(${PAD} 0)`}>
              <FreqGrid w={iw} h={h} />
            </g>
            <HRules w={w} rules={[{ y: y(0), axis: true }]} />
            <path className={p.fill} d={areaPath(points, h)} />
            <path className={p.live} d={linePath(points)} />
            <PlotHandle
              x={fx(toneHz)}
              y={clamp(y(stage(toneHz) - peak), PAD, h - PAD)}
              label={label}
              sliders={[tone]}
              onMove={(x) =>
                tone.setScaled(clamp(xFreq(x - PAD, iw), tone.properties.start, tone.properties.end))
              }
            />
          </>
        );
      }}
    </Plot>
  );
}
