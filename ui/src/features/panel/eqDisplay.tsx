/**
 * The EQ editor's display (docs/FX-ENGINES.md §4.4): the summed response of
 * everything the current type reads, as one curve on a log-frequency grid,
 * filled to the 0 dB line, with handles on the bands.
 *
 *   Classic     bass shelf 150 Hz (4 dB/unit), mid bell 425 Hz (3 dB/unit,
 *               Q 1.5 cutting / 0.7 boosting), treble shelf 1800 Hz (2 dB/unit)
 *   Parametric  low cut, the same shelves, a mid bell at Mid freq / Mid Q,
 *               tilt (first order, pivot 700 Hz), high cut, level
 *   Stack       the Yeh–Smith passive tone stack with the voice's parts and
 *               make-up, then level
 *   Graphic     seven octave bells, Q √2, then level
 *
 * The analog prototypes the engine matches (Vicanek bells, TPT shelves and
 * cuts, the Stack's own circuit), evaluated per pixel: these are the real laws.
 */
import { useCallback, useRef, useState, type PointerEvent as ReactPointerEvent } from "react";
import type { BlockId, ComboParamId, SliderParamId } from "../../bridge";
import { cx } from "../../components";
import { useComboParam, useSliderParam, type SliderParam } from "../../hooks";
import {
  FreqGrid,
  HRules,
  Plot,
  PlotHandle,
  areaPath,
  bellDb,
  clamp,
  curve,
  dbY,
  freqX,
  highCutDb,
  highShelfDb,
  linePath,
  lowCutDb,
  lowShelfDb,
  xFreq,
  plot as p,
  useEase,
} from "./display";
import { padFreq } from "./driveDisplay";
import eqInk from "./eq.module.css";

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
const BASS = { db: 4, hz: 150 };
const MID = { db: 3, hz: 425 };
const TREBLE = { db: 2, hz: 1800 };

/** First-order tilt about 700 Hz: −tilt dB at DC, +tilt dB at the top. */
function tiltDb(f: number, tilt: number): number {
  const t = Math.pow(10, tilt / 20);
  const s = f / 700;
  return 10 * Math.log10((t * t * s * s + 1) / (s * s + t * t));
}

/** Stack parts per voice (Tweed, Blackface, British, Top Boost):
 *  R1 treble, R2 bass, R3 mid, R4 slope, C1, C2, C3, make-up dB. */
const VOICES: readonly (readonly number[])[] = [
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
function stackResponse(voice: number, bass: number, mid: number, treble: number): (f: number) => number {
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

/** The noon dip of a voice, where its mid handle sits (150 Hz – 2.5 kHz). */
function stackDipHz(voice: number): number {
  const at = stackResponse(voice, 5, 5, 5);
  let best = 150;
  for (let f = 150; f <= 2500; f *= 1.02) if (at(f) < at(best)) best = f;
  return best;
}
const STACK_DIP = VOICES.map((_, v) => stackDipHz(v));
/** Where the stack's bass and treble handles sit: the frequencies the study
 *  measures each knob's range at (80 Hz, 5 kHz). */
const STACK_BASS_HZ = 80;
const STACK_TREBLE_HZ = 5000;

/* ───────────────────────────────── display ─────────────────────────────── */

/** Inset so a handle at 20 Hz or 20 kHz is not cut by the plot's edge. */
const PAD = 12;

const slider = (block: BlockId, key: string): SliderParamId => `${block}_${key}` as SliderParamId;

export function EqPlot({ block, type }: { block: BlockId; type: number }) {
  const bass = useSliderParam(slider(block, "bass"));
  const mid = useSliderParam(slider(block, "mid"));
  const treble = useSliderParam(slider(block, "treble"));
  const lowCut = useSliderParam(slider(block, "lowcut"));
  const highCut = useSliderParam(slider(block, "highcut"));
  const midFreq = useSliderParam(slider(block, "mid_freq"));
  const midQ = useSliderParam(slider(block, "mid_q"));
  const tilt = useSliderParam(slider(block, "tilt"));
  const level = useSliderParam(slider(block, "level"));
  const voice = useComboParam(`${block}_voice` as ComboParamId);
  const bands = [
    useSliderParam(slider(block, "g100")),
    useSliderParam(slider(block, "g200")),
    useSliderParam(slider(block, "g400")),
    useSliderParam(slider(block, "g800")),
    useSliderParam(slider(block, "g1k6")),
    useSliderParam(slider(block, "g3k2")),
    useSliderParam(slider(block, "g6k4")),
  ] as const;

  /* What is drawn travels to a value that changed without a hand on it (a
     preset, automation); see useEase. Frequencies travel on the log axis they
     are drawn on. The raw values still drive every handle's write. */
  const e = {
    bass: useEase(bass.scaled),
    mid: useEase(mid.scaled),
    treble: useEase(treble.scaled),
    lowCut: Math.exp(useEase(Math.log(lowCut.scaled))),
    highCut: Math.exp(useEase(Math.log(highCut.scaled))),
    midFreq: Math.exp(useEase(Math.log(midFreq.scaled))),
    midQ: useEase(midQ.scaled),
    tilt: useEase(tilt.scaled),
    level: useEase(level.scaled),
    bands: [
      useEase(bands[0].scaled),
      useEase(bands[1].scaled),
      useEase(bands[2].scaled),
      useEase(bands[3].scaled),
      useEase(bands[4].scaled),
      useEase(bands[5].scaled),
      useEase(bands[6].scaled),
    ],
  };
  /* A type or voice pick is discrete: the curve crossfades (in dB) from the
     old response to the new one, so the picture moves rather than cutting. */
  const typeW = [
    useEase(type === CLASSIC ? 1 : 0),
    useEase(type === PARAMETRIC ? 1 : 0),
    useEase(type === STACK ? 1 : 0),
    useEase(type === GRAPHIC ? 1 : 0),
  ];
  const voiceW = [
    useEase(voice.index === 0 ? 1 : 0),
    useEase(voice.index === 1 ? 1 : 0),
    useEase(voice.index === 2 ? 1 : 0),
    useEase(voice.index === 3 ? 1 : 0),
  ];
  /* ±12 dB bands get ±15; the knob bands reach ±20 dB, so ±24. */
  const RANGE = useEase(type === GRAPHIC ? 15 : 24);

  const knobDb = (value: number, perUnit: number): number => perUnit * (value - 5);
  const fromDb = (knob: SliderParam, db: number, perUnit: number): void =>
    knob.setScaled(clamp(5 + db / perUnit, knob.properties.start, knob.properties.end));

  /* The summed response (dB at f) of each type, from the eased values. */
  const stackShown = weighted(
    voiceW.map((wv, v) => [wv, stackResponse(v, e.bass, e.mid, e.treble)] as const),
  );
  const lowOn = e.lowCut > 20.5; /* the engine skips a cut parked at its end */
  const highOn = e.highCut < 19999;
  const classicMid = knobDb(e.mid, MID.db);
  const byType: readonly ((f: number) => number)[] = [
    (f) =>
      lowShelfDb(f, BASS.hz, knobDb(e.bass, BASS.db)) +
      bellDb(f, MID.hz, classicMid, classicMid < 0 ? 1.5 : 0.7) +
      highShelfDb(f, TREBLE.hz, knobDb(e.treble, TREBLE.db)),
    (f) =>
      (lowOn ? lowCutDb(f, e.lowCut) : 0) +
      lowShelfDb(f, BASS.hz, knobDb(e.bass, BASS.db)) +
      bellDb(f, e.midFreq, knobDb(e.mid, MID.db), e.midQ) +
      highShelfDb(f, TREBLE.hz, knobDb(e.treble, TREBLE.db)) +
      tiltDb(f, e.tilt) +
      (highOn ? highCutDb(f, e.highCut) : 0) +
      e.level,
    (f) => stackShown(f) + e.level,
    (f) => e.bands.reduce((sum, g, i) => sum + bellDb(f, GRAPHIC_BANDS[i]![1], g, Math.SQRT2), e.level),
  ];
  const response = weighted(typeW.map((wt, t) => [wt, byType[t]!] as const));
  /* The stack's mid handle sits at its voice's dip, travelling with the voice. */
  const stackDip = voiceW.reduce((sum, wv, v) => sum + wv * (STACK_DIP[v] ?? 500), 0) /
    Math.max(1e-6, voiceW.reduce((a, b) => a + b, 0));

  return (
    <Plot flex={1} aria-label="EQ response">
      {({ w, h }) => {
        const iw = w - 2 * PAD;
        const fx = (f: number): number => PAD + freqX(f, iw);
        const xf = (x: number): number => xFreq(x - PAD, iw);
        const y = (db: number): number => dbY(clamp(db, -RANGE - 6, RANGE + 6), h, RANGE, -RANGE);
        const yDb = (py: number): number => RANGE - (py / h) * 2 * RANGE;
        const hy = (db: number): number => clamp(y(db), PAD, h - PAD);
        const zero = y(0);
        const points = curve(w, (x) => y(response(padFreq(x, iw))));
        /* The handles of the type just picked arrive with its curve. */
        const handles = (t: number) => ({ opacity: Math.min(1, typeW[t] ?? 1) });

        /* The stack's knobs are pot positions, not dB: a handle sits on the
           curve at its band and the drag picks the knob value that puts the
           curve under the pointer there (from the raw values). */
        const stackHandle = (knob: SliderParam, f: number, which: 0 | 1 | 2, label: string) => {
          const at = (v: number): number => {
            const k = [bass.scaled, mid.scaled, treble.scaled];
            k[which] = v;
            return stackResponse(voice.index, k[0]!, k[1]!, k[2]!)(f) + level.scaled;
          };
          return (
            <PlotHandle
              key={label}
              x={fx(f)}
              y={hy(stackShown(f) + e.level)}
              label={label}
              sliders={[knob]}
              onMove={(_, py) => {
                const target = yDb(py);
                let best = knob.scaled;
                let err = Infinity;
                for (let v = 0; v <= 10.0001; v += 0.1) {
                  const d = Math.abs(at(v) - target);
                  if (d < err) [best, err] = [v, d];
                }
                knob.setScaled(best);
              }}
            />
          );
        };

        const gainHandle = (knob: SliderParam, shown: number, f: number, perUnit: number, label: string) => (
          <PlotHandle
            key={label}
            x={fx(f)}
            y={hy(knobDb(shown, perUnit))}
            label={label}
            sliders={[knob]}
            onMove={(_, py) => fromDb(knob, yDb(py), perUnit)}
          />
        );

        const cutHandle = (knob: SliderParam, shown: number, label: string) => (
          <PlotHandle
            key={label}
            x={fx(shown)}
            y={hy(response(shown))}
            label={label}
            sliders={[knob]}
            onMove={(x) => knob.setScaled(clamp(xf(x), knob.properties.start, knob.properties.end))}
          />
        );

        const graphicGrid = typeW[GRAPHIC] ?? 0;
        return (
          <>
            {/* Graphic: the bands are the frequency axis, a line under each;
                otherwise decades. Crossfaded with the type. */}
            {graphicGrid > 0.001 && (
              <g opacity={graphicGrid}>
                {GRAPHIC_BANDS.map(([key, f]) => {
                  const x = Math.round(fx(f)) + 0.5;
                  return <line key={key} className={p.grid} x1={x} x2={x} y1={0} y2={h} />;
                })}
              </g>
            )}
            {graphicGrid < 0.999 && (
              <g transform={`translate(${PAD} 0)`} opacity={1 - graphicGrid}>
                <FreqGrid w={iw} h={h} />
              </g>
            )}
            <HRules
              w={w}
              rules={[
                { y: y(12), label: "+12" },
                { y: zero, axis: true },
                { y: y(-12), label: "-12" },
              ]}
            />
            <path className={p.fill} d={areaPath(points, zero)} />
            <path className={p.live} d={linePath(points)} />

            {type === CLASSIC && (
              <g style={handles(CLASSIC)}>
                {gainHandle(bass, e.bass, BASS.hz, BASS.db, "Bass")}
                {gainHandle(mid, e.mid, MID.hz, MID.db, "Mid")}
                {gainHandle(treble, e.treble, TREBLE.hz, TREBLE.db, "Treble")}
              </g>
            )}

            {type === STACK && (
              <g style={handles(STACK)}>
                {stackHandle(bass, STACK_BASS_HZ, 0, "Bass")}
                {stackHandle(mid, stackDip, 1, "Mid")}
                {stackHandle(treble, STACK_TREBLE_HZ, 2, "Treble")}
              </g>
            )}

            {type === PARAMETRIC && (
              <g style={handles(PARAMETRIC)}>
                {cutHandle(lowCut, e.lowCut, "Low cut")}
                {cutHandle(highCut, e.highCut, "High cut")}
                {gainHandle(bass, e.bass, BASS.hz, BASS.db, "Bass")}
                {gainHandle(treble, e.treble, TREBLE.hz, TREBLE.db, "Treble")}
                <PlotHandle
                  x={fx(e.midFreq)}
                  y={hy(knobDb(e.mid, MID.db))}
                  label="Mid and Mid freq"
                  sliders={[mid, midFreq]}
                  onMove={(x, py) => {
                    midFreq.setScaled(clamp(xf(x), midFreq.properties.start, midFreq.properties.end));
                    fromDb(mid, yDb(py), MID.db);
                  }}
                />
              </g>
            )}

            {type === GRAPHIC && (
              <g style={handles(GRAPHIC)}>
                {GRAPHIC_BANDS.map(([key, f, tick], i) => {
                  const band = bands[i]!;
                  /* The hit column spans the band's octave: half-way to each neighbour. */
                  const left = fx(f / Math.SQRT2);
                  const right = fx(f * Math.SQRT2);
                  return (
                    <BandHandle
                      key={key}
                      x={fx(f)}
                      left={left}
                      right={right}
                      zero={zero}
                      y={hy(e.bands[i]!)}
                      h={h}
                      tick={tick}
                      label={`${tick} Hz`}
                      band={band}
                      onMove={(py) =>
                        band.setScaled(clamp(yDb(py), band.properties.start, band.properties.end))
                      }
                    />
                  );
                })}
              </g>
            )}
          </>
        );
      }}
    </Plot>
  );
}

/** A blend of responses by weight (normalised), skipping the ones at rest at 0:
 *  at rest it is exactly the one response whose weight is 1. */
function weighted(parts: readonly (readonly [number, (f: number) => number])[]): (f: number) => number {
  const live = parts.filter(([wt]) => wt > 1e-3);
  const total = live.reduce((sum, [wt]) => sum + wt, 0);
  if (live.length === 1 || total <= 0) {
    const only = live[0]?.[1] ?? parts[0]![1];
    return only;
  }
  return (f) => live.reduce((sum, [wt, fn]) => sum + wt * fn(f), 0) / total;
}

/**
 * One graphic-EQ band: a stem from the 0 dB line to a handle at the band's
 * gain, its frequency under it, and a hit column the width of the band's
 * octave — press anywhere in the column to set the gain there and drag.
 * Double-click resets. (PlotHandle's 24 px round hit area is too small for
 * seven bands in a row; this is its column-shaped sibling.)
 */
function BandHandle({
  x,
  left,
  right,
  zero,
  y,
  h,
  tick,
  label,
  band,
  onMove,
}: {
  x: number;
  left: number;
  right: number;
  zero: number;
  y: number;
  h: number;
  tick: string;
  label: string;
  band: SliderParam;
  onMove(py: number): void;
}) {
  const dragging = useRef(false);
  const [active, setActive] = useState(false);

  const position = useCallback(
    (e: ReactPointerEvent<SVGGElement>) => {
      const svg = e.currentTarget.ownerSVGElement;
      if (svg) onMove(e.clientY - svg.getBoundingClientRect().top);
    },
    [onMove],
  );

  const end = useCallback(
    (e: ReactPointerEvent<SVGGElement>) => {
      if (!dragging.current) return;
      dragging.current = false;
      setActive(false);
      if (e.currentTarget.hasPointerCapture(e.pointerId)) e.currentTarget.releasePointerCapture(e.pointerId);
      band.endGesture();
    },
    [band],
  );

  return (
    <g
      className={cx(p.handle, eqInk.band, active && p.handleActive, active && eqInk.bandActive)}
      role="slider"
      aria-label={label}
      onPointerDown={(e) => {
        if (e.button !== 0) return;
        e.stopPropagation();
        e.currentTarget.setPointerCapture(e.pointerId);
        dragging.current = true;
        setActive(true);
        band.beginGesture();
        position(e);
      }}
      onPointerMove={(e) => {
        if (dragging.current) position(e);
      }}
      onPointerUp={end}
      onPointerCancel={end}
      onDoubleClick={() => band.reset()}
    >
      <title>{label}</title>
      <rect className={p.handleHit} x={left} y={0} width={right - left} height={h} />
      <line className={eqInk.stem} x1={x} x2={x} y1={zero} y2={y} />
      <text className={p.tick} x={x} y={h - 6} textAnchor="middle">
        {tick}
      </text>
      <circle className={p.handleDot} cx={x} cy={y} r={4.5} />
    </g>
  );
}
