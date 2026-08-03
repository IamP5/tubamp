/**
 * Horizontal linear slider bound to a slider relay — used for `amp_slim`, the
 * one native `LinearHorizontal` control in the block panel.
 *
 * Like the Knob primitive, the fill and readout are driven by a MotionValue and
 * a direct DOM write, so dragging never re-renders React (motion-design.md §4.4).
 */
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type PointerEvent as ReactPointerEvent,
} from "react";
import { motion, useMotionValue, useTransform } from "motion/react";
import { bridge, type SliderParamId, type SliderProperties } from "../../bridge";
import { defaultScaledValue, formatParamValue } from "../../bridge/paramMeta";
import s from "./panel.module.css";

export interface ParamSliderProps {
  id: SliderParamId;
  /** Micro-label to the left of the track. */
  label: string;
  disabled?: boolean;
}

export function ParamSlider({ id, label, disabled }: ParamSliderProps) {
  const state = useMemo(() => bridge.sliderState(id), [id]);
  const [properties, setProperties] = useState<SliderProperties>(() => ({
    ...state.properties,
  }));
  const norm = useMotionValue(state.getNormalisedValue());
  const width = useTransform(norm, (v) => `${(v * 100).toFixed(2)}%`);
  const readoutRef = useRef<HTMLSpanElement>(null);
  const trackRef = useRef<HTMLDivElement>(null);
  const dragging = useRef(false);

  const paintReadout = useCallback(() => {
    if (readoutRef.current) {
      readoutRef.current.textContent = formatParamValue(
        state.getScaledValue(),
        state.properties,
      );
    }
  }, [state]);

  /* Backend → UI: automation, preset loads and our own writes all land here. */
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
      state.setNormalisedValue(Math.min(1, Math.max(0, next)));
      norm.set(state.getNormalisedValue());
      paintReadout();
    },
    [state, norm, paintReadout],
  );

  const writeFromClientX = useCallback(
    (clientX: number) => {
      const el = trackRef.current;
      if (!el) return;
      const rect = el.getBoundingClientRect();
      if (rect.width <= 0) return;
      write((clientX - rect.left) / rect.width);
    },
    [write],
  );

  const onPointerDown = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      if (disabled || e.button !== 0) return;
      e.currentTarget.setPointerCapture(e.pointerId);
      dragging.current = true;
      state.beginGesture();
      writeFromClientX(e.clientX);
    },
    [disabled, state, writeFromClientX],
  );

  const onPointerMove = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      if (!dragging.current) return;
      writeFromClientX(e.clientX);
    },
    [writeFromClientX],
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

  return (
    <div
      className={disabled ? `${s.field} ${s.fieldDisabled}` : s.field}
      data-param-index={properties.parameterIndex}
    >
      <span className={s.fieldLabel}>{label}</span>
      <div
        ref={trackRef}
        className={s.sliderTrack}
        role="slider"
        aria-label={label}
        aria-valuemin={properties.start}
        aria-valuemax={properties.end}
        aria-disabled={disabled}
        title={properties.name}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
        onDoubleClick={onDoubleClick}
      >
        <motion.div className={s.sliderFill} style={{ width }} />
      </div>
      <span ref={readoutRef} className={s.readout} />
    </div>
  );
}
