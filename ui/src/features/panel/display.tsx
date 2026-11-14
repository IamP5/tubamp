/**
 * The plotting toolkit behind every editor's display (ui/DESIGN.md §Editors).
 *
 * A display draws what the effect is doing — a decay, a transfer curve, a
 * frequency response, an LFO — from the same parameters the knobs edit, and
 * offers handles for the two or three edits that are easier to make on the
 * picture than on a knob. It never prints a parameter's number: the knob does.
 *
 *   <Plot aria-label="Reverb decay">
 *     {({ w, h }) => (
 *       <>
 *         <FreqGrid w={w} h={h} />
 *         <path className={p.fill} d={…} />
 *         <path className={p.live} d={…} />
 *         <PlotHandle x={…} y={…} label="Decay" sliders={[decay]} onMove={(x) => …} />
 *       </>
 *     )}
 *   </Plot>
 *
 * Ink (./display.module.css): `p.grid` / `p.axis` for scaffolding, `p.ghost`
 * for a reference curve, `p.live` + `p.fill` for THE curve (the accent, in the
 * block's family hue; the fill fades towards the baseline), `p.tick` for axis
 * text, `p.meter` for live signal.
 *
 * Two kinds of motion, both of them data:
 *  - `useEase(value)`: a value that changed without a hand on it (a type pick,
 *    a preset, automation) arrives over 180 ms, so the picture moves to its new
 *    shape instead of cutting to it. While a pointer is down it is the raw
 *    value, so a drag never lags.
 *  - `useLive(paint)`: per-frame DOM writes from the player's input
 *    (`Signal`): `p.energy` over a fill, `p.dot` riding a curve. Off, with a
 *    static picture, under reduced motion.
 */
import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type PointerEvent as ReactPointerEvent,
  type ReactNode,
} from "react";
import { useReducedMotion } from "motion/react";
import type { SliderParamId } from "../../bridge";
import { cx } from "../../components";
import { onFrame, useSignal, useSliderParam, type Signal, type SliderParam } from "../../hooks";
import { duration } from "../../theme/motion";
import p from "./display.module.css";
import { highCutDb, lowCutDb } from "./filterDb";

export { p as plot };

/* ───────────────────────────────── motion ──────────────────────────────── */

/* Whether a pointer is down anywhere: a value that changes under a hand is
   followed exactly; only a change nobody is holding is eased. One capturing
   listener set for the app, installed on first use. */
let pointersDown = 0;
let watchingPointers = false;
function watchPointers(): void {
  if (watchingPointers || typeof window === "undefined") return;
  watchingPointers = true;
  const up = (): void => {
    pointersDown = Math.max(0, pointersDown - 1);
  };
  window.addEventListener("pointerdown", () => (pointersDown += 1), true);
  window.addEventListener("pointerup", up, true);
  window.addEventListener("pointercancel", up, true);
  window.addEventListener("blur", () => (pointersDown = 0));
}

/** `--ds-ease`, near enough, for values eased in JS. */
const easeOut = (k: number): number => 1 - Math.pow(1 - k, 3);

interface Eased {
  target: number;
  from: number;
  /** Progress 0..1; 1 means at rest on `target`. */
  k: number;
}

/**
 * `target`, eased. When it changes with no pointer down, the returned value
 * travels there over `--ds-t-base`; otherwise it is `target` on the same
 * render. Re-renders its component per frame only while travelling.
 */
export function useEase(target: number): number {
  const reduced = useReducedMotion() ?? false;
  const [state, setState] = useState<Eased>({ target, from: target, k: 1 });
  watchPointers();

  let now = state;
  if (state.target !== target) {
    const shown = state.from + (state.target - state.from) * easeOut(state.k);
    const travel = !reduced && pointersDown === 0 && Number.isFinite(shown) && Number.isFinite(target);
    now = { target, from: travel ? shown : target, k: travel ? 0 : 1 };
    setState(now);
  }

  const travelling = now.k < 1;
  useEffect(() => {
    if (!travelling) return;
    return onFrame((dt) =>
      setState((prev) => (prev.k >= 1 ? prev : { ...prev, k: Math.min(1, prev.k + dt / duration.base) })),
    );
  }, [travelling]);

  return now.from + (now.target - now.from) * easeOut(now.k);
}

/**
 * Runs `paint` every frame with the player's input level history, for marks
 * that follow the signal. `paint` writes to the DOM (an opacity, a transform)
 * through refs; it must not set React state. Not run at all under reduced
 * motion, so the marks keep whatever the markup gave them (hidden).
 */
export function useLive(paint: (signal: Signal, dt: number) => void): void {
  const reduced = useReducedMotion() ?? false;
  const signal = useSignal();
  const latest = useRef(paint);
  useEffect(() => {
    latest.current = paint;
  });
  useEffect(() => {
    if (reduced) return;
    return onFrame((dt) => latest.current(signal, dt));
  }, [reduced, signal]);
}

/**
 * The fade under `p.fill`, defined once per editor (ui.tsx renders it): the
 * accent at the curve, almost nothing at the baseline. The rule that points
 * `p.fill` at it is written into the document here rather than in the CSS
 * module, because a fragment `url()` in a stylesheet file resolves against the
 * stylesheet in older WebKit, and against the document everywhere when the
 * rule is inline. Without this component `p.fill` is the flat token.
 */
export function PlotDefs() {
  return (
    <svg className={p.defs} width={0} height={0} aria-hidden>
      <defs>
        <linearGradient id="ds-plot-fade" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0" className={p.fadeTop} />
          <stop offset="1" className={p.fadeBottom} />
        </linearGradient>
      </defs>
      <style>{`.${p.fill}{fill:url(#ds-plot-fade)}`}</style>
    </svg>
  );
}

export interface PlotSize {
  w: number;
  h: number;
}

/**
 * A plot that fills its parent and hands its real pixel size to the render
 * function, so strokes and text never stretch. Several plots can sit side by
 * side in one display: give each a `flex` and separate them with `PlotSplit`.
 */
export function Plot({
  children,
  flex = 1,
  ...rest
}: {
  children: (size: PlotSize) => ReactNode;
  /** Share of the display's width when plots sit side by side. */
  flex?: number;
  "aria-label": string;
}) {
  const [box, setBox] = useState<HTMLDivElement | null>(null);
  const [size, setSize] = useState<PlotSize>({ w: 0, h: 0 });

  useEffect(() => {
    if (!box) return;
    const measure = (): void => {
      const next = { w: box.clientWidth, h: box.clientHeight };
      setSize((prev) => (prev.w === next.w && prev.h === next.h ? prev : next));
    };
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(box);
    return () => observer.disconnect();
  }, [box]);

  return (
    <div ref={setBox} className={p.plot} style={{ flex }}>
      {size.w > 0 && size.h > 0 && (
        <svg className={p.svg} width={size.w} height={size.h} role="img" aria-label={rest["aria-label"]}>
          {children(size)}
        </svg>
      )}
    </div>
  );
}

/** The hairline between two plots in one display. */
export function PlotSplit() {
  return <span className={p.split} aria-hidden />;
}

/* ───────────────────────────────── scales ──────────────────────────────── */

export const clamp = (v: number, lo: number, hi: number): number => Math.min(hi, Math.max(lo, v));

/** Linear map of `v` in [a, b] onto [c, d] (not clamped). */
export const lerp = (v: number, a: number, b: number, c: number, d: number): number =>
  c + ((v - a) / (b - a)) * (d - c);

export const F_MIN = 20;
export const F_MAX = 20000;

/** Frequency to x on a log axis spanning the plot's width. */
export function freqX(f: number, w: number, fMin = F_MIN, fMax = F_MAX): number {
  return (Math.log(clamp(f, fMin, fMax) / fMin) / Math.log(fMax / fMin)) * w;
}

/** x to frequency, the inverse of `freqX`. */
export function xFreq(x: number, w: number, fMin = F_MIN, fMax = F_MAX): number {
  return fMin * Math.pow(fMax / fMin, clamp(x / w, 0, 1));
}

/** dB to y: `top` dB at the plot's top edge, `bottom` dB at its bottom edge. */
export function dbY(db: number, h: number, top: number, bottom: number): number {
  return lerp(db, top, bottom, 0, h);
}

/** An SVG path through `points` (already in pixels). */
export function linePath(points: readonly (readonly [number, number])[]): string {
  return points.map(([x, y], i) => `${i === 0 ? "M" : "L"}${x.toFixed(1)} ${y.toFixed(1)}`).join("");
}

/** The same line closed down to `baseline`, for the fill under a curve. */
export function areaPath(points: readonly (readonly [number, number])[], baseline: number): string {
  if (points.length === 0) return "";
  const first = points[0]!;
  const last = points[points.length - 1]!;
  return `${linePath(points)}L${last[0].toFixed(1)} ${baseline}L${first[0].toFixed(1)} ${baseline}Z`;
}

/** Samples `fn(x)` every `step` px across the width: the points of a curve. */
export function curve(w: number, fn: (x: number) => number, step = 2): [number, number][] {
  const points: [number, number][] = [];
  for (let x = 0; x <= w; x += step) points.push([x, fn(x)]);
  if (points.length === 0 || points[points.length - 1]![0] !== w) points.push([w, fn(w)]);
  return points;
}

/* ──────────────────────────── filter magnitudes ────────────────────────── */

export { bellDb, highCutDb, highShelfDb, lowCutDb, lowShelfDb } from "./filterDb";

/* ───────────────────────────────── scaffolding ─────────────────────────── */

const FREQ_TICKS: readonly (readonly [number, string])[] = [
  [100, "100"],
  [1000, "1k"],
  [10000, "10k"],
];

/** Decade lines and their labels for a log-frequency plot. */
export function FreqGrid({ w, h, labels = true }: PlotSize & { labels?: boolean }) {
  return (
    <g>
      {FREQ_TICKS.map(([f, text]) => {
        const x = Math.round(freqX(f, w)) + 0.5;
        return (
          <g key={f}>
            <line className={p.grid} x1={x} x2={x} y1={0} y2={h} />
            {labels && (
              <text className={p.tick} x={x + 4} y={h - 6}>
                {text}
              </text>
            )}
          </g>
        );
      })}
    </g>
  );
}

/** Horizontal rules at the given y positions, optionally labelled at the left. */
export function HRules({
  w,
  rules,
}: {
  w: number;
  rules: readonly { y: number; label?: string; axis?: boolean }[];
}) {
  return (
    <g>
      {rules.map(({ y, label, axis }) => {
        const yy = Math.round(y) + 0.5;
        return (
          <g key={`${yy}:${label ?? ""}`}>
            <line className={axis ? p.axis : p.grid} x1={0} x2={w} y1={yy} y2={yy} />
            {label && (
              <text className={p.tick} x={6} y={yy - 4}>
                {label}
              </text>
            )}
          </g>
        );
      })}
    </g>
  );
}

/* ───────────────────────────────── handle ──────────────────────────────── */

export interface PlotHandleProps {
  x: number;
  y: number;
  /** Accessible name and tooltip: the parameter(s) this handle edits. */
  label: string;
  /** The parameters the drag writes, so one drag is one host gesture each and a
   *  double-click resets them. */
  sliders: readonly SliderParam[];
  /** Pointer position in the plot's own pixels while dragging. */
  onMove(x: number, y: number): void;
  /** Dim and lock (the engine ignores the parameter right now). */
  disabled?: boolean;
}

/**
 * A draggable point on a plot. Neutral at rest, accent while hovered or
 * dragged, with a 24px hit area. Double-click resets its parameters.
 */
export function PlotHandle({ x, y, label, sliders, onMove, disabled }: PlotHandleProps) {
  const dragging = useRef(false);
  const [active, setActive] = useState(false);

  const position = useCallback(
    (e: ReactPointerEvent<SVGGElement>) => {
      const svg = e.currentTarget.ownerSVGElement;
      if (!svg) return;
      const rect = svg.getBoundingClientRect();
      onMove(e.clientX - rect.left, e.clientY - rect.top);
    },
    [onMove],
  );

  const end = useCallback(
    (e: ReactPointerEvent<SVGGElement>) => {
      if (!dragging.current) return;
      dragging.current = false;
      setActive(false);
      if (e.currentTarget.hasPointerCapture(e.pointerId)) {
        e.currentTarget.releasePointerCapture(e.pointerId);
      }
      for (const slider of sliders) slider.endGesture();
    },
    [sliders],
  );

  return (
    <g
      className={cx(p.handle, active && p.handleActive, disabled && p.handleDisabled)}
      transform={`translate(${x.toFixed(1)} ${y.toFixed(1)})`}
      role="slider"
      aria-label={label}
      onPointerDown={(e) => {
        if (disabled || e.button !== 0) return;
        e.stopPropagation();
        e.currentTarget.setPointerCapture(e.pointerId);
        dragging.current = true;
        setActive(true);
        for (const slider of sliders) slider.beginGesture();
      }}
      onPointerMove={(e) => {
        if (dragging.current) position(e);
      }}
      onPointerUp={end}
      onPointerCancel={end}
      onDoubleClick={() => {
        if (!disabled) for (const slider of sliders) slider.reset();
      }}
    >
      <title>{label}</title>
      <circle className={p.handleHit} r={12} />
      <circle className={p.handleDot} r={4.5} />
    </g>
  );
}

/* ───────────────────────────── shared displays ─────────────────────────── */

/**
 * Low cut + high cut as one frequency response with a handle on each corner.
 * Shared by every block that has the pair (cab, delay, reverb).
 */
export function CutPlot({
  lowCut,
  highCut,
  flex,
  disabled,
  label = "Filter response",
}: {
  lowCut: SliderParamId;
  highCut: SliderParamId;
  flex?: number;
  disabled?: boolean;
  label?: string;
}) {
  const lo = useSliderParam(lowCut);
  const hi = useSliderParam(highCut);
  const loHz = useEase(lo.scaled);
  const hiHz = useEase(hi.scaled);
  /* 0 dB sits at the same height as in a neighbouring level-against-time plot
     (6 dB of headroom over a 60 dB range), so the two axes line up. */
  const TOP = 3;
  const BOTTOM = -30;

  return (
    <Plot flex={flex} aria-label={label}>
      {({ w, h }) => {
        const y = (db: number): number => dbY(db, h, TOP, BOTTOM);
        const points = curve(w, (x) => {
          const f = xFreq(x, w);
          return y(Math.max(BOTTOM - 6, lowCutDb(f, loHz) + highCutDb(f, hiHz)));
        });
        return (
          <>
            <FreqGrid w={w} h={h} />
            <HRules w={w} rules={[{ y: y(0), axis: true }]} />
            <path className={p.fill} d={areaPath(points, h)} />
            <path className={cx(p.live, disabled && p.liveOff)} d={linePath(points)} />
            <PlotHandle
              x={freqX(loHz, w)}
              y={y(-3)}
              label="Low cut"
              sliders={[lo]}
              disabled={disabled}
              onMove={(x) => lo.setScaled(clamp(xFreq(x, w), lo.properties.start, lo.properties.end))}
            />
            <PlotHandle
              x={freqX(hiHz, w)}
              y={y(-3)}
              label="High cut"
              sliders={[hi]}
              disabled={disabled}
              onMove={(x) => hi.setScaled(clamp(xFreq(x, w), hi.properties.start, hi.properties.end))}
            />
          </>
        );
      }}
    </Plot>
  );
}
