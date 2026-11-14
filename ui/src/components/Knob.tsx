import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type PointerEvent as ReactPointerEvent,
} from "react";
import { animate, useMotionValue, useReducedMotion } from "motion/react";
import s from "./kit.module.css";
import { cx } from "./cx";
import { bridge, type SliderParamId, type SliderProperties } from "../bridge";
import { defaultScaledValue } from "../bridge/paramMeta";
import { tween } from "../theme/motion";
import { paramLabel, paramValue } from "../theme/words";
import { MinusIcon, PlusIcon } from "./icons";

/** 270° sweep, 0° at twelve o'clock, clockwise. */
const SWEEP_DEG = 270;
const START_DEG = -135;
/** Pixels of vertical travel for the full range. */
const DRAG_RANGE_PX = 200;
const FINE_FACTOR = 0.2;
/** A value that arrives from outside (a preset, a type's starting points, a
 *  reset) and moves the dial by more than this share of its travel is swept to
 *  rather than cut to. Anything smaller — automation, a neighbour's link — is
 *  followed exactly. */
const SWEEP_FROM = 0.02;
/** Value arc stroke, and the gap between it and the disc's edge. */
const ARC_W = 2;
const ARC_GAP = 2;

export type KnobSize = "sm" | "md" | "lg";

/** Outer box of the dial, arc included. */
const DIAMETER: Record<KnobSize, number> = { sm: 32, md: 40, lg: 52 };

const SIZE_CLASS: Record<KnobSize, string> = {
  sm: s.knobSm,
  md: s.knobMd,
  lg: s.knobLg,
};

export interface KnobProps {
  /** Tooltip; defaults to the parameter's name. Use it for the one fact about
   *  this control that changes how it is used. */
  title?: string;
  /** APVTS parameter id — the knob binds itself to the relay. */
  id: SliderParamId;
  /** Label under the dial (run through `paramLabel`); defaults to the backend's
   *  name. Also the slider's aria-label, verbatim. */
  label?: string;
  /** Dial box: sm 32, md 40 (default), lg 52. */
  size?: KnobSize;
  /** Accepted for old call sites and ignored: there is one accent. */
  accent?: string;
  /** Hide the readout (and with it the step buttons). */
  hideValue?: boolean;
  /** Hide the label under the dial. */
  hideLabel?: boolean;
  /** Replaces the numeric readout (tempo-synced rate shows its note value). The
   *  dial still moves and reads the parameter; only the text differs. */
  valueText?: string;
  disabled?: boolean;
  className?: string;
  /** Custom readout text; defaults to `paramValue`. Keep it a stable function. */
  format?: (scaled: number) => string;
}

function clamp01(v: number): number {
  return Math.min(1, Math.max(0, v));
}

/** Point on the dial at `deg` (0 = twelve o'clock, clockwise). */
function polar(c: number, r: number, deg: number): [number, number] {
  const a = (deg * Math.PI) / 180;
  return [c + r * Math.sin(a), c - r * Math.cos(a)];
}

/**
 * Rotary knob (ui/DESIGN.md §Components, Knob). A flat `surface-3` disc with
 * a hairline, a 1.5px pointer, and a 2px value arc just outside the disc. The
 * arc is the accent (the block's family hue inside an editor); the pointer is
 * neutral at rest and joins it while the knob is hovered or dragged. A
 * bipolar parameter (start === −end) fills its arc from the centre. Label and
 * readout below; `−` / `+` step buttons flank the readout on hover and while
 * dragging.
 *
 * Interaction: vertical drag (shift = fine), double-click resets to the
 * default, wheel nudges, the step buttons move one step. Every change is
 * wrapped in begin/endGesture so host automation records a clean touch.
 *
 * Performance: dragging does not re-render React. The value lives in a
 * MotionValue; the pointer, arc and readout are painted by direct DOM writes.
 * React state only holds the async parameter *properties*.
 */
export function Knob({
  title,
  id,
  label,
  size = "md",
  hideValue,
  hideLabel,
  valueText,
  disabled,
  className,
  format,
}: KnobProps) {
  const state = useMemo(() => bridge.sliderState(id), [id]);
  const [properties, setProperties] = useState<SliderProperties>(() => ({
    ...state.properties,
  }));
  const norm = useMotionValue(state.getNormalisedValue());
  const rootRef = useRef<HTMLDivElement>(null);
  const dialRef = useRef<HTMLDivElement>(null);
  const readoutRef = useRef<HTMLSpanElement>(null);
  const pointerRef = useRef<SVGLineElement>(null);
  const valueRef = useRef<SVGPathElement>(null);
  const dragging = useRef(false);
  const dragStart = useRef({ y: 0, value: 0 });
  const sweep = useRef<{ stop(): void } | null>(null);
  const stopSweep = useCallback((): void => {
    sweep.current?.stop();
    sweep.current = null;
  }, []);
  const reduced = useReducedMotion() ?? false;

  /* ── geometry ── */
  const d = DIAMETER[size];
  const c = d / 2;
  const arcR = c - ARC_W / 2;
  const discR = arcR - ARC_W / 2 - ARC_GAP - 0.5;
  const arcLength = arcR * ((SWEEP_DEG * Math.PI) / 180);
  const [ax, ay] = polar(c, arcR, START_DEG);
  const [bx, by] = polar(c, arcR, START_DEG + SWEEP_DEG);
  const arcPath = `M${ax} ${ay}A${arcR} ${arcR} 0 1 1 ${bx} ${by}`;
  const pointerFrom = discR * 0.35;
  const pointerTo = discR - 2.5;

  /* Where the arc fills from: the centre for a bipolar range, else the start. */
  const { start, end, skew } = properties;
  const bipolar = end > 0 && start === -end;
  const origin = bipolar ? Math.pow(-start / (end - start), skew) : 0;

  const paintDial = useCallback(
    (v: number) => {
      pointerRef.current?.setAttribute(
        "transform",
        `rotate(${START_DEG + v * SWEEP_DEG} ${c} ${c})`,
      );
      const a = Math.min(v, origin);
      const b = Math.max(v, origin);
      valueRef.current?.setAttribute(
        "stroke-dasharray",
        `0 ${a * arcLength} ${(b - a) * arcLength} ${arcLength * 2}`,
      );
    },
    [arcLength, c, origin],
  );

  const paintReadout = useCallback(() => {
    const scaled = state.getScaledValue();
    const text =
      valueText ?? (format ? format(scaled) : paramValue(id, scaled, state.properties));
    if (readoutRef.current) readoutRef.current.textContent = text;
    if (dialRef.current) {
      dialRef.current.setAttribute("aria-valuenow", String(scaled));
      dialRef.current.setAttribute("aria-valuetext", text);
    }
  }, [format, id, state, valueText]);

  /* Paint before the first frame, and again whenever geometry or the origin
     changes (a size change, new properties). */
  useLayoutEffect(() => {
    paintDial(norm.get());
    return norm.on("change", paintDial);
  }, [norm, paintDial]);

  /* Keep the motion value and readout in sync with the backend (automation,
     preset loads, our own writes) without re-rendering. */
  useEffect(() => {
    const sync = (): void => {
      const next = state.getNormalisedValue();
      stopSweep();
      if (!reduced && !dragging.current && Math.abs(next - norm.get()) > SWEEP_FROM) {
        sweep.current = animate(norm, next, tween.crossfade);
      } else {
        norm.set(next);
      }
      paintReadout();
    };
    sync();
    const offValue = state.onValueChanged(sync);
    const offProps = state.onPropertiesChanged(() => {
      setProperties({ ...state.properties });
      sync();
    });
    return () => {
      offValue();
      offProps();
      stopSweep();
    };
  }, [state, norm, paintReadout, reduced, stopSweep]);

  const write = useCallback(
    (next: number) => {
      stopSweep();
      state.setNormalisedValue(clamp01(next));
      // The backend may have echoed the write back through `sync` already.
      stopSweep();
      norm.set(state.getNormalisedValue());
      paintReadout();
    },
    [state, norm, paintReadout, stopSweep],
  );

  /** Sets the parameter and sweeps the dial there: for a jump the user asked
   *  for without dragging (a reset). */
  const settle = useCallback(
    (next: number) => {
      state.setNormalisedValue(clamp01(next));
      const target = state.getNormalisedValue();
      stopSweep();
      if (reduced) norm.set(target);
      else sweep.current = animate(norm, target, tween.crossfade);
      paintReadout();
    },
    [state, norm, paintReadout, reduced, stopSweep],
  );

  const setActive = (active: boolean): void => {
    const el = rootRef.current;
    if (!el) return;
    if (active) el.setAttribute("data-active", "true");
    else el.removeAttribute("data-active");
  };

  const onPointerDown = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      if (disabled || e.button !== 0) return;
      e.currentTarget.setPointerCapture(e.pointerId);
      dragging.current = true;
      dragStart.current = { y: e.clientY, value: norm.get() };
      setActive(true);
      state.beginGesture();
    },
    [disabled, norm, state],
  );

  const onPointerMove = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      if (!dragging.current) return;
      const dy = dragStart.current.y - e.clientY;
      const factor = e.shiftKey ? FINE_FACTOR : 1;
      write(dragStart.current.value + (dy / DRAG_RANGE_PX) * factor);
    },
    [write],
  );

  const endDrag = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      if (!dragging.current) return;
      dragging.current = false;
      if (e.currentTarget.hasPointerCapture(e.pointerId)) {
        e.currentTarget.releasePointerCapture(e.pointerId);
      }
      setActive(false);
      state.endGesture();
    },
    [state],
  );

  const onDoubleClick = useCallback(() => {
    if (disabled) return;
    const p = state.properties;
    const proportion = (defaultScaledValue(id) - p.start) / (p.end - p.start);
    state.beginGesture();
    settle(Math.pow(clamp01(proportion), p.skew));
    state.endGesture();
  }, [disabled, id, state, settle]);

  /**
   * One step. A parameter with at most 100 steps moves by its interval; a
   * finer one moves 1% of its (skewed) travel, but always at least one
   * interval so snapping cannot swallow the click.
   */
  const step = useCallback(
    (dir: 1 | -1) => {
      if (disabled) return;
      const p = state.properties;
      const range = p.end - p.start;
      if (range <= 0) return;
      const toNorm = (scaled: number): number =>
        Math.pow(clamp01((scaled - p.start) / range), p.skew);
      const fromNorm = (n: number): number =>
        p.start + Math.pow(clamp01(n), 1 / p.skew) * range;
      const current = state.getScaledValue();
      let target: number;
      if (p.interval > 0 && range / p.interval <= 100) {
        target = current + dir * p.interval;
      } else {
        target = fromNorm(norm.get() + dir * 0.01);
        if (p.interval > 0 && Math.abs(target - current) < p.interval) {
          target = current + dir * p.interval;
        }
      }
      state.beginGesture();
      write(toNorm(Math.min(p.end, Math.max(p.start, target))));
      state.endGesture();
    },
    [disabled, norm, state, write],
  );

  /* Wheel nudge: one parameter step, or a fine step with shift. */
  useEffect(() => {
    const el = dialRef.current;
    if (!el || disabled) return;
    const onWheel = (e: WheelEvent): void => {
      // A sideways swipe is not a nudge: it is the user scrolling whatever the
      // knob sits in (the amp panel scrolls horizontally when the TONE3000
      // drawer narrows it). Left alone it would also read as "up" — deltaY is 0.
      if (e.deltaY === 0 || Math.abs(e.deltaX) > Math.abs(e.deltaY)) return;
      e.preventDefault();
      e.stopPropagation();
      const steps = Math.max(properties.numSteps - 1, 1);
      const delta = (1 / Math.min(steps, 400)) * (e.shiftKey ? FINE_FACTOR : 1);
      const dir = e.deltaY > 0 ? -1 : 1;
      state.beginGesture();
      write(norm.get() + dir * delta);
      state.endGesture();
    };
    el.addEventListener("wheel", onWheel, { passive: false });
    return () => el.removeEventListener("wheel", onWheel);
  }, [disabled, properties.numSteps, state, norm, write]);

  const text = paramLabel(label ?? properties.name);

  return (
    <div
      ref={rootRef}
      className={cx(s.knob, SIZE_CLASS[size], disabled && s.knobDisabled, className)}
      data-param-index={properties.parameterIndex}
      title={title ?? properties.name}
    >
      <div
        ref={dialRef}
        className={s.knobDial}
        style={{ width: d, height: d }}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
        onDoubleClick={onDoubleClick}
        role="slider"
        aria-label={label ?? properties.name}
        aria-valuemin={properties.start}
        aria-valuemax={properties.end}
        aria-disabled={disabled || undefined}
      >
        <svg className={s.knobSvg} width={d} height={d} viewBox={`0 0 ${d} ${d}`}>
          <path className={s.knobTrack} d={arcPath} strokeWidth={ARC_W} />
          <path
            ref={valueRef}
            className={s.knobArc}
            d={arcPath}
            strokeWidth={ARC_W}
            strokeDasharray={`0 ${arcLength * 2}`}
          />
          <circle className={s.knobDisc} cx={c} cy={c} r={discR} />
          <line
            ref={pointerRef}
            className={s.knobPointer}
            x1={c}
            y1={c - pointerFrom}
            x2={c}
            y2={c - pointerTo}
          />
        </svg>
      </div>
      {!hideLabel && <span className={s.knobLabel}>{text}</span>}
      {!hideValue && (
        <span className={s.knobValueRow}>
          <button
            type="button"
            className={cx(s.knobStep, s.knobStepDec)}
            aria-label={`Decrease ${text}`}
            tabIndex={-1}
            disabled={disabled}
            onClick={() => step(-1)}
          >
            <MinusIcon size={12} strokeWidth={2} />
          </button>
          <span ref={readoutRef} className={s.knobReadout} />
          <button
            type="button"
            className={cx(s.knobStep, s.knobStepInc)}
            aria-label={`Increase ${text}`}
            tabIndex={-1}
            disabled={disabled}
            onClick={() => step(1)}
          >
            <PlusIcon size={12} strokeWidth={2} />
          </button>
        </span>
      )}
    </div>
  );
}
