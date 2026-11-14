/**
 * Compressor editor (docs/FX-ENGINES.md §4.2). Instance-aware: each compressor
 * block edits its own `<token>_*` parameters and reads its own meter slot.
 * Classic reads five knobs (Threshold, Ratio, Attack, Release, Makeup) and no
 * Auto makeup; the rest dim in place. Picking a type moves Knee, Attack and
 * Release to that type's starting points.
 *
 * Display: the static transfer curve, input level against output level, from
 * Compressor.cpp's own law — the soft knee (Giannoulis et al.; hard in
 * Classic), FET's threshold rising with the ratio, makeup and auto makeup
 * lifting the wet path, the parallel mix — with the unity line as reference
 * and a handle on the threshold. Squash computes its gain from the output, but
 * its steady state is the same curve (Compressor.cpp `feedbackGain`). Gain
 * reduction is the bar at the right edge.
 *
 * Live: a point rides the curve where the input is right now — placed from the
 * engine's real gain reduction while it compresses, from the input level below
 * the threshold — and fades out in silence.
 */
import { useRef } from "react";
import {
  instanceOf,
  type BlockId,
  type ComboParamId,
  type SliderParamId,
  type ToggleParamId,
} from "../../bridge";
import { COMP_GR_SCALE_DB, useComboParam, useDynMeters, useSliderParam, useToggleParam } from "../../hooks";
import { Plot, PlotHandle, areaPath, clamp, curve, lerp, linePath, plot as p, useEase, useLive } from "./display";
import { DYN_PAD_Y, GrMeter } from "./DynMeters";
import { COMP_STARTING_POINTS, applyStartingPoints } from "./typeDefaults";
import { Editor, Group, HeaderSlot, K, Row, SwitchField, TypeStepper } from "./ui";
import d from "./dyn.module.css";

const CLASSIC = 0;
const SQUASH = 2;
const FET = 3;

export function CompBody({ block = "comp" }: { block?: BlockId }) {
  const id = (key: string): SliderParamId => `${block}_${key}` as SliderParamId;
  const type = useComboParam(`${block}_type` as ComboParamId);
  const classic = type.index === CLASSIC;

  return (
    <>
      <HeaderSlot>
        <TypeStepper
          id={`${block}_type` as ComboParamId}
          label="Type"
          onPick={(index) => applyStartingPoints(block, COMP_STARTING_POINTS, index)}
        />
        <SwitchField id={`${block}_auto_makeup` as ToggleParamId} label="Auto makeup" disabled={classic} />
      </HeaderSlot>
      <Editor display={<CompDisplay block={block} type={type.index} />}>
        <Row>
          <K id={id("threshold")} label="Threshold" size="lg" />
          <K id={id("ratio")} label="Ratio" size="lg" />
          <K id={id("makeup")} label="Makeup" size="lg" />
        </Row>
        <Row>
          <Group>
            <K id={id("attack")} label="Attack" />
            <K id={id("release")} label="Release" />
            <K
              id={id("knee")}
              label="Knee"
              disabled={classic}
              tip={
                type.index === SQUASH
                  ? "Squash reads its own output level; the knee still spans this much input level"
                  : classic
                    ? "Classic has a hard knee"
                    : undefined
              }
            />
            <K id={id("mix")} label="Mix" disabled={classic} />
            <div className={d.lastWide}>
              <K id={id("sc_hpf")} label="Sidechain HPF" disabled={classic} />
            </div>
          </Group>
        </Row>
      </Editor>
    </>
  );
}

/* ───────────────────────────────── display ─────────────────────────────── */

const IN_MIN = -60;
const DB_MAX = 0;
const GRID = [-48, -36, -24, -12] as const;
const LABELS = new Set([-48, -24]);
const GR_TICKS = [0, 5, 10, 15, 20] as const;
/** Inset at the left, so a handle at -60 dB is not cut by the plot's edge. */
const PAD_X = 12;

/* Compressor.cpp constants. */
const FET_OFFSET_DB_PER_OCTAVE = 2.6;
const FET_OFFSET_FROM_RATIO = 4;
const AUTO_MAKEUP_REFERENCE_DB = -10;
const MAX_TOTAL_MAKEUP_DB = 48;

/** Compressor.cpp `gainComputer`: gain (dB <= 0) for a level `o` dB over the
 *  threshold, slope s = 1/R - 1, total knee width `wk` dB. */
function gainComputer(o: number, s: number, wk: number): number {
  if (2 * o <= -wk) return 0;
  if (2 * o < wk) {
    const u = o + 0.5 * wk;
    return (s * u * u) / (2 * wk);
  }
  return s * o;
}

interface Law {
  threshold: number;
  slope: number;
  knee: number;
  /** Makeup applied to the wet path, dB (manual + auto). */
  makeup: number;
  /** Wet share, 0..1. */
  wet: number;
}

/** Steady-state output level for input level `x` (both dBFS). */
function transfer(x: number, law: Law): number {
  const wetDb = x + gainComputer(x - law.threshold, law.slope, law.knee) + law.makeup;
  if (law.wet >= 1) return wetDb;
  const lin = law.wet * Math.pow(10, wetDb / 20) + (1 - law.wet) * Math.pow(10, x / 20);
  return 20 * Math.log10(Math.max(lin, 1e-9));
}

/** FET's threshold rise for ratio `r` (Compressor.cpp). */
function fetOffset(type: number, r: number): number {
  return type === FET && r > FET_OFFSET_FROM_RATIO ? FET_OFFSET_DB_PER_OCTAVE * Math.log2(r / FET_OFFSET_FROM_RATIO) : 0;
}

/**
 * The input level (dBFS) at which the law reduces by `gr` dB: `gainComputer`
 * inverted. Over the knee it is threshold + GR / (1 − 1/R); inside the knee
 * the quadratic's root. Ratio 1 never reduces, so it has no answer (NaN).
 */
function inputForGr(gr: number, law: Law): number {
  const s = -law.slope;
  if (s <= 1e-6) return NaN;
  const wk = law.knee;
  if (wk > 0 && gr < (s * wk) / 2) return law.threshold + Math.sqrt((2 * wk * gr) / s) - 0.5 * wk;
  return law.threshold + gr / s;
}

/** Gain reduction under this counts as none (the meters fall towards 0). */
const GR_EPSILON_DB = 0.05;
/** Input level (0..1 of useSignal's 48 dB window) at which the point is fully shown. */
const DOT_FULL_LEVEL = 0.15;

function CompDisplay({ block, type }: { block: BlockId; type: number }) {
  const meters = useDynMeters();
  const grValue = meters.compGr[instanceOf(block)] ?? meters.compGr[0];
  const sl = (key: string) => `${block}_${key}` as SliderParamId;
  const threshold = useSliderParam(sl("threshold"));
  const ratio = useSliderParam(sl("ratio"));
  const knee = useSliderParam(sl("knee"));
  const makeup = useSliderParam(sl("makeup"));
  const mix = useSliderParam(sl("mix"));
  const auto = useToggleParam(`${block}_auto_makeup` as ToggleParamId);

  const classic = type === CLASSIC;
  /* The handle writes through the raw offset; the picture draws eased values
     (a type pick or preset moves the curve over 180 ms; see useEase). Derived
     targets are eased too, so Classic's hard knee and full wet path, FET's
     offset and the Auto makeup switch travel rather than cut. */
  const rawOffset = fetOffset(type, Math.max(1, ratio.scaled));
  const r = Math.max(1, useEase(ratio.scaled));
  const offset = useEase(rawOffset);
  const manualMakeup = useEase(makeup.scaled);
  const autoShare = useEase(!classic && auto.value ? 1 : 0);
  const law: Law = {
    threshold: useEase(threshold.scaled) + offset,
    slope: 1 / r - 1,
    knee: Math.max(0, useEase(classic ? 0 : knee.scaled)),
    makeup: manualMakeup,
    wet: clamp(useEase(classic ? 1 : mix.scaled / 100), 0, 1),
  };
  if (autoShare > 0) {
    const autoDb = -gainComputer(AUTO_MAKEUP_REFERENCE_DB - law.threshold, law.slope, law.knee);
    law.makeup += autoShare * Math.min(autoDb, MAX_TOTAL_MAKEUP_DB - manualMakeup);
  }

  /* The operating point: where the input sits on the curve right now. From
     the engine's own gain reduction while it compresses (the law inverted);
     below that from the plugin's input level, an approximation that is never
     placed past where reduction would begin. Hidden in silence. */
  const dot = useRef<SVGCircleElement>(null);
  useLive((signal) => {
    const el = dot.current;
    if (!el) return;
    const w = Number(el.dataset.w);
    const h = Number(el.dataset.h);
    const gr = Math.max(0, grValue.get());
    const level = signal.now();
    const knot = law.threshold - 0.5 * law.knee;
    const fromGr = gr > GR_EPSILON_DB ? inputForGr(gr, law) : NaN;
    const fromLevel = Math.min(knot, -48 + 48 * level);
    const xIn = clamp(Number.isFinite(fromGr) ? fromGr : fromLevel, IN_MIN, DB_MAX);
    const opacity = Math.max(clamp(level / DOT_FULL_LEVEL, 0, 1), gr > GR_EPSILON_DB ? 1 : 0);
    el.style.opacity = opacity.toFixed(3);
    if (opacity <= 0) return;
    const px = lerp(xIn, IN_MIN, DB_MAX, PAD_X, w);
    const py = clamp(lerp(transfer(xIn, law), DB_MAX, IN_MIN, DYN_PAD_Y, h - DYN_PAD_Y), 0, h);
    el.setAttribute("transform", `translate(${px.toFixed(1)} ${py.toFixed(1)})`);
  });

  return (
    <>
      <Plot aria-label="Compressor transfer curve">
        {({ w, h }) => {
          const x = (db: number): number => lerp(db, IN_MIN, DB_MAX, PAD_X, w);
          const xDb = (px: number): number => lerp(px, PAD_X, w, IN_MIN, DB_MAX);
          const y = (db: number): number => clamp(lerp(db, DB_MAX, IN_MIN, DYN_PAD_Y, h - DYN_PAD_Y), 0, h);
          const points = curve(w - PAD_X, (px) => y(transfer(xDb(px + PAD_X), law))).map(
            ([px, py]): [number, number] => [px + PAD_X, py],
          );
          const knot = clamp(law.threshold, IN_MIN, DB_MAX);
          const bottom = Math.round(y(IN_MIN)) + 0.5;
          return (
            <>
              {GRID.map((db) => {
                const gx = Math.round(x(db)) + 0.5;
                const gy = Math.round(y(db)) + 0.5;
                return (
                  <g key={db}>
                    <line className={p.grid} x1={gx} x2={gx} y1={0} y2={h} />
                    <line className={p.grid} x1={0} x2={w} y1={gy} y2={gy} />
                    {LABELS.has(db) && (
                      <>
                        <text className={p.tick} x={gx + 4} y={h - 6}>
                          {`−${-db}`}
                        </text>
                        <text className={p.tick} x={PAD_X - 6} y={gy - 4}>
                          {`−${-db}`}
                        </text>
                      </>
                    )}
                  </g>
                );
              })}
              <line className={p.axis} x1={0} x2={w} y1={bottom} y2={bottom} />
              <text className={p.caption} x={w - 6} y={h - 6} textAnchor="end">
                In
              </text>
              <text className={p.caption} x={PAD_X - 6} y={DYN_PAD_Y + 12}>
                Out
              </text>
              <line className={p.ghost} x1={x(IN_MIN)} y1={y(IN_MIN)} x2={x(DB_MAX)} y2={y(DB_MAX)} />
              <path className={p.fill} d={areaPath(points, h)} />
              <path className={p.live} d={linePath(points)} />
              <PlotHandle
                x={x(knot)}
                y={y(transfer(knot, law))}
                label="Threshold"
                sliders={[threshold]}
                onMove={(px) =>
                  threshold.setScaled(
                    clamp(xDb(px) - rawOffset, threshold.properties.start, threshold.properties.end),
                  )
                }
              />
              <circle ref={dot} className={p.dot} r={3.5} data-w={w} data-h={h} />
            </>
          );
        }}
      </Plot>
      <GrMeter value={grValue} scaleDb={COMP_GR_SCALE_DB} ticks={GR_TICKS} />
    </>
  );
}
