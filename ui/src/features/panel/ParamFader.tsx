/**
 * Vertical bipolar slider bound to a slider relay — the graphic EQ's band
 * faders. The vertical sibling of ParamSlider, in the panel's own style: the fill
 * grows from the centre line (0 dB) towards the thumb.
 *
 * Like ParamSlider, the fill, thumb and readout are driven by a MotionValue and a
 * direct DOM write, so dragging never re-renders React.
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
import { defaultScaledValue } from "../../bridge/paramMeta";
import s from "./panel.module.css";

export interface ParamFaderProps {
  id: SliderParamId;
  /** Micro-label under the track ("100", "1.6k"). */
  label: string;
}

/** "+3.2", "-6.0", "0.0": signed, no unit (the band is always dB). */
function formatSigned(value: number): string {
  const text = value.toFixed(1);
  return value > 0.05 ? `+${text}` : text === "-0.0" ? "0.0" : text;
}

export function ParamFader({ id, label }: ParamFaderProps) {
  const state = useMemo(() => bridge.sliderState(id), [id]);
  const [properties, setProperties] = useState<SliderProperties>(() => ({
    ...state.properties,
  }));
  const norm = useMotionValue(state.getNormalisedValue());
  /* Fill spans centre -> thumb: `top` is the thumb's edge above centre, else 50%. */
  const fillTop = useTransform(norm, (v) => `${((v >= 0.5 ? 1 - v : 0.5) * 100).toFixed(2)}%`);
  const fillHeight = useTransform(norm, (v) => `${(Math.abs(v - 0.5) * 100).toFixed(2)}%`);
  const thumbTop = useTransform(norm, (v) => `${((1 - v) * 100).toFixed(2)}%`);
  const readoutRef = useRef<HTMLSpanElement>(null);
  const trackRef = useRef<HTMLDivElement>(null);
  const dragging = useRef(false);

  const paintReadout = useCallback(() => {
    if (readoutRef.current) {
      readoutRef.current.textContent = formatSigned(state.getScaledValue());
    }
  }, [state]);

  /* Backend -> UI: automation, preset loads and our own writes all land here. */
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

  const writeFromClientY = useCallback(
    (clientY: number) => {
      const el = trackRef.current;
      if (!el) return;
      const rect = el.getBoundingClientRect();
      if (rect.height <= 0) return;
      write(1 - (clientY - rect.top) / rect.height);
    },
    [write],
  );

  const onPointerDown = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      if (e.button !== 0) return;
      e.currentTarget.setPointerCapture(e.pointerId);
      dragging.current = true;
      state.beginGesture();
      writeFromClientY(e.clientY);
    },
    [state, writeFromClientY],
  );

  const onPointerMove = useCallback(
    (e: ReactPointerEvent<HTMLDivElement>) => {
      if (!dragging.current) return;
      writeFromClientY(e.clientY);
    },
    [writeFromClientY],
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
    const { start, end, skew } = state.properties;
    const proportion = (defaultScaledValue(id) - start) / (end - start);
    state.beginGesture();
    write(Math.pow(Math.min(1, Math.max(0, proportion)), skew));
    state.endGesture();
  }, [id, state, write]);

  return (
    <div className={s.fader} data-param-index={properties.parameterIndex} title={properties.name}>
      <span ref={readoutRef} className={s.faderReadout} />
      <div
        ref={trackRef}
        className={s.faderTrack}
        role="slider"
        aria-label={label}
        aria-orientation="vertical"
        aria-valuemin={properties.start}
        aria-valuemax={properties.end}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
        onDoubleClick={onDoubleClick}
      >
        <span className={s.faderCentre} />
        <motion.div className={s.faderFill} style={{ top: fillTop, height: fillHeight }} />
        <motion.span className={s.faderThumb} style={{ top: thumbTop }} />
      </div>
      <span className={s.faderLabel}>{label}</span>
    </div>
  );
}
