import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type PointerEvent as ReactPointerEvent,
} from "react";
import { motion, useMotionValue, useTransform } from "motion/react";
import s from "./kit.module.css";
import { cx } from "./cx";
import { bridge, type SliderParamId, type SliderProperties } from "../bridge";
import { defaultScaledValue, formatParamValue } from "../bridge/paramMeta";
import { spring } from "../theme/motion";

/** 270° sweep, like the native rotary and the Geist spinner arc. */
const SWEEP_DEG = 270;
const START_DEG = -135;
/** Pixels of vertical travel for the full range. */
const DRAG_RANGE_PX = 200;
const FINE_FACTOR = 0.2;

export type KnobSize = "sm" | "md" | "lg";

const SIZES: Record<KnobSize, { d: number; stroke: number; pointer: number }> = {
  sm: { d: 40, stroke: 3, pointer: 10 },
  md: { d: 56, stroke: 4, pointer: 14 },
  lg: { d: 72, stroke: 5, pointer: 18 },
};

export interface KnobProps {
  /** APVTS parameter id — the knob binds itself to the relay. */
  id: SliderParamId;
  /** Uppercase micro-label under the dial; defaults to the backend's name. */
  label?: string;
  size?: KnobSize;
  /** Explicit accent; by default inherits `--block-accent` from a `[data-block]` ancestor. */
  accent?: string;
  /** Hide the numeric readout (footer knobs that show their value elsewhere). */
  hideValue?: boolean;
  /**
   * Ceiling for the READOUT only, in the parameter's own units: the dial and the
   * parameter still travel their full range, but the number stops where the DSP
   * stops clamping (docs/REVERB.md §5, `reverb_decay`'s per-mode ceiling).
   */
  displayMax?: number;
  disabled?: boolean;
  className?: string;
}

/**
 * Rotary knob, 270° sweep.
 *
 * Interaction: vertical drag (shift = fine), double-click resets to the
 * parameter's default, wheel nudges one step. Gestures are wrapped in
 * begin/endGesture so host automation records a clean touch.
 *
 * Performance: the dial angle, arc and readout text are driven by MotionValues
 * and direct DOM writes — dragging a knob does not re-render React (guardrail
 * §4.4). React state here only holds the async parameter *properties*.
 */
export function Knob({
  id,
  label,
  size = "md",
  accent,
  hideValue,
  displayMax,
  disabled,
  className,
}: KnobProps) {
  const state = useMemo(() => bridge.sliderState(id), [id]);
  const [properties, setProperties] = useState<SliderProperties>(() => ({
    ...state.properties,
  }));
  const norm = useMotionValue(state.getNormalisedValue());
  const readoutRef = useRef<HTMLSpanElement>(null);
  const dialRef = useRef<HTMLDivElement>(null);
  const dragging = useRef(false);
  const dragStart = useRef({ y: 0, value: 0 });

  const paintReadout = useCallback(() => {
    const scaled = state.getScaledValue();
    const shown = displayMax === undefined ? scaled : Math.min(scaled, displayMax);
    const text = formatParamValue(shown, state.properties);
    if (readoutRef.current) readoutRef.current.textContent = text;
    if (dialRef.current) {
      dialRef.current.setAttribute("aria-valuenow", String(scaled));
      dialRef.current.setAttribute("aria-valuetext", text);
    }
  }, [state, displayMax]);

  /* Keep the motion value + readout in sync with the backend (automation,
     preset loads, our own writes) without re-rendering. */
  useEffect(() => {
    const sync = (): void => {
      norm.set(state.getNormalisedValue());
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
    };
  }, [state, norm, paintReadout]);

  const write = useCallback(
    (next: number) => {
      const clamped = Math.min(1, Math.max(0, next));
      state.setNormalisedValue(clamped);
      norm.set(state.getNormalisedValue());
      paintReadout();
    },
    [state, norm, paintReadout],
  );

  const onPointerDown = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      if (disabled || e.button !== 0) return;
      e.currentTarget.setPointerCapture(e.pointerId);
      dragging.current = true;
      dragStart.current = { y: e.clientY, value: norm.get() };
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
      state.endGesture();
    },
    [state],
  );

  const onDoubleClick = useCallback(() => {
    if (disabled) return;
    const { start, end, skew } = state.properties;
    const proportion = (defaultScaledValue(id) - start) / (end - start);
    state.beginGesture();
    write(Math.pow(Math.min(1, Math.max(0, proportion)), skew));
    state.endGesture();
  }, [disabled, id, state, write]);

  /* Wheel nudge: one parameter step, or a fine step with shift. */
  useEffect(() => {
    const el = dialRef.current;
    if (!el || disabled) return;
    const onWheel = (e: WheelEvent): void => {
      e.preventDefault();
      e.stopPropagation();
      const steps = Math.max(properties.numSteps - 1, 1);
      const step = (1 / Math.min(steps, 400)) * (e.shiftKey ? FINE_FACTOR : 1);
      const dir = e.deltaY > 0 ? -1 : 1;
      state.beginGesture();
      write(norm.get() + dir * step);
      state.endGesture();
    };
    el.addEventListener("wheel", onWheel, { passive: false });
    return () => el.removeEventListener("wheel", onWheel);
  }, [disabled, properties.numSteps, state, norm, write]);

  const { d, stroke, pointer } = SIZES[size];
  const radius = (d - stroke) / 2;
  const circumference = 2 * Math.PI * radius;
  const arcLength = (SWEEP_DEG / 360) * circumference;

  const rotate = useTransform(norm, (v) => START_DEG + v * SWEEP_DEG);
  const dashOffset = useTransform(norm, (v) => arcLength * (1 - v));

  const style = accent
    ? ({ "--block-accent": accent } as React.CSSProperties)
    : undefined;

  return (
    <div
      className={cx(s.knob, size === "sm" && s.knobSizeSm, disabled && s.knobDisabled, className)}
      style={style}
      data-param-index={properties.parameterIndex}
      title={properties.name}
    >
      <motion.div
        ref={dialRef}
        className={s.knobDial}
        style={{ width: d, height: d }}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
        onDoubleClick={onDoubleClick}
        whileTap={{ scale: 1.06 }}
        transition={spring.micro}
        role="slider"
        aria-label={label ?? properties.name}
        aria-valuemin={properties.start}
        aria-valuemax={properties.end}
      >
        <svg
          className={s.knobSvg}
          width={d}
          height={d}
          viewBox={`0 0 ${d} ${d}`}
          style={{ transform: `rotate(${90 + (360 - SWEEP_DEG) / 2}deg)` }}
        >
          <circle
            className={s.knobTrack}
            cx={d / 2}
            cy={d / 2}
            r={radius}
            fill="none"
            strokeWidth={stroke}
            strokeDasharray={`${arcLength} ${circumference}`}
            strokeLinecap="round"
          />
          <motion.circle
            className={s.knobValue}
            cx={d / 2}
            cy={d / 2}
            r={radius}
            fill="none"
            strokeWidth={stroke}
            strokeDasharray={`${arcLength} ${circumference}`}
            style={{ strokeDashoffset: dashOffset }}
            strokeLinecap="round"
          />
        </svg>
        {/* Full-size layer so the pointer pivots around the dial centre. */}
        <motion.div style={{ position: "absolute", inset: 0, rotate }}>
          <span
            style={{
              position: "absolute",
              left: "50%",
              top: stroke + 4,
              marginLeft: -1,
              width: 2,
              height: pointer,
              borderRadius: 1,
              background: "var(--block-accent, var(--accent))",
            }}
          />
        </motion.div>
      </motion.div>
      <span className={s.knobLabel}>{label ?? properties.name}</span>
      {!hideValue && <span ref={readoutRef} className={s.knobReadout} />}
    </div>
  );
}
