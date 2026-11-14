/**
 * The EQ editor's display (docs/FX-ENGINES.md §4.4): the summed response of
 * everything the current type reads (the laws in ./eqResponse), as one curve on
 * a log-frequency grid, filled to the 0 dB line, with handles on the bands.
 *
 * Behind the curve, while the Analyzer switch is on and audio plays, the
 * spectrum (useEqSpectrum): what leaves the block as a neutral area, what
 * enters it as a dimmer line, shown only where the two differ. Its scale is
 * its own and unlabelled (FabFilter Pro-Q's analyser): +4.5 dB per octave of
 * tilt about 1 kHz, 0 dB at the top, -90 dB at the bottom, smoothed over about
 * 1/12 octave.
 *
 * The analog prototypes the engine matches (Vicanek bells, TPT shelves and
 * cuts, the Stack's own circuit), evaluated per pixel: these are the real laws.
 */
import { useCallback, useEffect, useRef, useState, type PointerEvent as ReactPointerEvent } from "react";
import type { BlockId, ComboParamId, EqInstance, SliderParamId } from "../../bridge";
import { cx } from "../../components";
import {
  EQ_SPECTRUM_POINTS,
  eqSpectrumHz,
  useComboParam,
  useEqSpectrum,
  useSliderParam,
  type EqSpectrumFrame,
  type SliderParam,
} from "../../hooks";
import { useStore } from "../../store";
import {
  FreqGrid,
  HRules,
  Plot,
  PlotHandle,
  areaPath,
  clamp,
  curve,
  dbY,
  freqX,
  linePath,
  xFreq,
  plot as p,
  useEase,
} from "./display";
import { padFreq } from "./driveDisplay";
import {
  BASS,
  GRAPHIC,
  GRAPHIC_BANDS,
  MID,
  PARAMETRIC,
  STACK,
  TREBLE,
  CLASSIC,
  VOICES,
  classicResponse,
  graphicResponse,
  knobDb,
  parametricResponse,
  stackResponse,
} from "./eqResponse";
import eqInk from "./eq.module.css";

export { CLASSIC, GRAPHIC, GRAPHIC_BANDS, PARAMETRIC, STACK } from "./eqResponse";

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

/** The EQ instance a block token edits (eq, eq2, eq3). */
const instanceOf = (block: BlockId): EqInstance => (block === "eq3" ? 2 : block === "eq2" ? 1 : 0);

export function EqPlot({ block, type }: { block: BlockId; type: number }) {
  const analyzer = useStore((st) => st.eqAnalyser.on);
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

  const fromDb = (knob: SliderParam, db: number, perUnit: number): void =>
    knob.setScaled(clamp(5 + db / perUnit, knob.properties.start, knob.properties.end));

  /* The summed response (dB at f) of each type, from the eased values. */
  const stackShown = weighted(
    voiceW.map((wv, v) => [wv, stackResponse(v, e.bass, e.mid, e.treble)] as const),
  );
  const byType: readonly ((f: number) => number)[] = [
    classicResponse(e),
    parametricResponse(e),
    (f) => stackShown(f) + e.level,
    graphicResponse(e),
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
            {analyzer && <EqSpectrum instance={instanceOf(block)} w={w} h={h} />}
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

/* ───────────────────────────────── spectrum ────────────────────────────── */

/** Display tilt, dB per octave about `SPECTRUM_PIVOT_HZ` (Pro-Q's default), so
 *  a guitar's falling spectrum reads level rather than as a slope. */
const SPECTRUM_TILT_DB_PER_OCTAVE = 4.5;
const SPECTRUM_PIVOT_HZ = 1000;
/** The spectrum's own scale: 0 dB (after tilt) at the plot's top, this many dB
 *  below at its bottom, whatever the curve's gain range. */
const SPECTRUM_RANGE_DB = 90;
/** Smoothing across frequency: a binomial kernel over ±2 points (the points are
 *  ~0.04 octave apart, so about 1/12 octave wide at half height). */
const SPECTRUM_KERNEL = [1 / 16, 4 / 16, 6 / 16, 4 / 16, 1 / 16] as const;
/** The pre line is drawn only once it is this far from post somewhere: a flat
 *  or bypassed EQ shows one shape, not two on top of each other. */
const SPECTRUM_SAME_DB = 0.5;

const SPECTRUM_TILT = Float32Array.from({ length: EQ_SPECTRUM_POINTS }, (_, i) =>
  SPECTRUM_TILT_DB_PER_OCTAVE * Math.log2(eqSpectrumHz(i) / SPECTRUM_PIVOT_HZ),
);

/** `src` smoothed across frequency and tilted, into `out` (display dB). */
function present(src: Float32Array, out: Float32Array): void {
  const last = EQ_SPECTRUM_POINTS - 1;
  for (let i = 0; i <= last; i += 1) {
    let sum = 0;
    for (let k = 0; k < SPECTRUM_KERNEL.length; k += 1) {
      sum += SPECTRUM_KERNEL[k]! * src[clamp(i + k - 2, 0, last)]!;
    }
    out[i] = sum + SPECTRUM_TILT[i]!;
  }
}

/**
 * The analyser behind the curve: post as `p.spectrum` with `p.spectrumLine` on
 * its edge, pre as `p.spectrumPre`. The paths are written per frame through
 * refs (useEqSpectrum), never through React state; nothing is drawn in silence.
 */
function EqSpectrum({ instance, w, h }: { instance: EqInstance; w: number; h: number }) {
  const area = useRef<SVGPathElement>(null);
  const line = useRef<SVGPathElement>(null);
  const preLine = useRef<SVGPathElement>(null);
  const scratch = useRef({
    pre: new Float32Array(EQ_SPECTRUM_POINTS),
    post: new Float32Array(EQ_SPECTRUM_POINTS),
  });

  /* x per point, on the curve's own padded axis; changes only with the size. */
  const xs = useRef<Float32Array>(new Float32Array(EQ_SPECTRUM_POINTS));

  const paint = (frame: EqSpectrumFrame | null): void => {
    const nodes = [area.current, line.current, preLine.current];
    if (!frame) {
      for (const node of nodes) node?.setAttribute("d", "");
      return;
    }
    const { pre, post } = scratch.current;
    present(frame.pre, pre);
    present(frame.post, post);
    const y = (db: number): number => clamp((-db / SPECTRUM_RANGE_DB) * h, 0, h);
    const x = xs.current;

    let visible = false;
    let differ = false;
    for (let i = 0; i < EQ_SPECTRUM_POINTS; i += 1) {
      const a = post[i]!;
      const b = pre[i]!;
      if (a > -SPECTRUM_RANGE_DB || b > -SPECTRUM_RANGE_DB) {
        visible = true;
        /* Only where it would show: below the bottom both are just floor. */
        if (Math.abs(a - b) > SPECTRUM_SAME_DB) differ = true;
      }
    }
    if (!visible) {
      for (const node of nodes) node?.setAttribute("d", "");
      return;
    }

    /* The first and last points are held out to the plot's edges, like the curve. */
    const trace = (db: Float32Array): string => {
      let d = `M0 ${y(db[0]!).toFixed(1)}`;
      for (let i = 0; i < EQ_SPECTRUM_POINTS; i += 1) d += `L${x[i]!.toFixed(1)} ${y(db[i]!).toFixed(1)}`;
      return `${d}L${w} ${y(db[EQ_SPECTRUM_POINTS - 1]!).toFixed(1)}`;
    };
    const postD = trace(post);
    area.current?.setAttribute("d", `${postD}L${w} ${h}L0 ${h}Z`);
    line.current?.setAttribute("d", postD);
    preLine.current?.setAttribute("d", differ ? trace(pre) : "");
  };

  const speed = useStore((st) => st.eqAnalyser.speed);
  const freeze = useStore((st) => st.eqFreeze);
  const resolution = useStore((st) => st.eqAnalyser.resolution);
  const repaint = useEqSpectrum(instance, paint, { speed, freeze, resolution });
  useEffect(() => {
    for (let i = 0; i < EQ_SPECTRUM_POINTS; i += 1) xs.current[i] = PAD + freqX(eqSpectrumHz(i), w - 2 * PAD);
    repaint();
  }, [w, h, repaint]);

  return (
    <g aria-hidden data-spectrum="">
      <path ref={area} className={p.spectrum} d="" />
      <path ref={preLine} className={p.spectrumPre} d="" />
      <path ref={line} className={p.spectrumLine} d="" />
    </g>
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
