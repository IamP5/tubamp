/**
 * Slider parameter binding.
 *
 * Subscribes to BOTH `valueChanged` and `propertiesChanged` — properties (range,
 * skew, unit label, parameterIndex) arrive asynchronously from the backend, so a
 * hook that only listens to values renders a 0..1 linear range on first paint.
 *
 * Gestures: call `beginGesture()` before the first write of a drag and
 * `endGesture()` after the last, or host automation records no touch.
 *
 * Note for hot paths: this hook re-renders on every value change. For a control
 * dragged at pointer rate (the Knob primitive) drive a MotionValue from the raw
 * `bridge.sliderState(id)` instead — see components/Knob.tsx.
 */
import { useCallback, useMemo, useSyncExternalStore } from "react";
import { bridge, type SliderParamId, type SliderProperties } from "../bridge";
import { defaultScaledValue, formatParamValue } from "../bridge/paramMeta";

export interface SliderParam {
  /** Normalised 0..1 (skew applied) — what the knob geometry uses. */
  value: number;
  /** Parameter units (dB / Hz / ms / …). */
  scaled: number;
  /** Formatted readout, JUCE default formatting + unit suffix. */
  text: string;
  properties: SliderProperties;
  /** "Input Trim" — the backend's parameter name. */
  name: string;
  /** "dB", "Hz", ":1" … */
  label: string;
  /** For the `data-param-index` annotation (Logic touch-to-select). */
  parameterIndex: number;
  setNormalised(value: number): void;
  setScaled(value: number): void;
  /** Double-click-to-reset target, from the static param table. */
  reset(): void;
  beginGesture(): void;
  endGesture(): void;
  /** One-shot write wrapped in a begin/end gesture. */
  nudge(deltaNormalised: number): void;
}

export function useSliderParam(id: SliderParamId): SliderParam {
  const state = useMemo(() => bridge.sliderState(id), [id]);

  const subscribe = useCallback(
    (onChange: () => void) => {
      const offValue = state.onValueChanged(onChange);
      const offProps = state.onPropertiesChanged(onChange);
      return () => {
        offValue();
        offProps();
      };
    },
    [state],
  );

  const scaled = useSyncExternalStore(subscribe, () => state.getScaledValue());
  const properties = useSyncExternalStore(subscribe, () => state.properties);
  const value = useSyncExternalStore(subscribe, () =>
    state.getNormalisedValue(),
  );

  const setNormalised = useCallback(
    (v: number) => state.setNormalisedValue(Math.min(1, Math.max(0, v))),
    [state],
  );

  const setScaled = useCallback(
    (scaledValue: number) => {
      const { start, end, skew } = state.properties;
      const proportion = (scaledValue - start) / (end - start);
      setNormalised(Math.pow(Math.min(1, Math.max(0, proportion)), skew));
    },
    [state, setNormalised],
  );

  const beginGesture = useCallback(() => state.beginGesture(), [state]);
  const endGesture = useCallback(() => state.endGesture(), [state]);

  const reset = useCallback(() => {
    state.beginGesture();
    setScaled(defaultScaledValue(id));
    state.endGesture();
  }, [id, state, setScaled]);

  const nudge = useCallback(
    (delta: number) => {
      state.beginGesture();
      setNormalised(state.getNormalisedValue() + delta);
      state.endGesture();
    },
    [state, setNormalised],
  );

  return {
    value,
    scaled,
    text: formatParamValue(scaled, properties),
    properties,
    name: properties.name,
    label: properties.label,
    parameterIndex: properties.parameterIndex,
    setNormalised,
    setScaled,
    reset,
    beginGesture,
    endGesture,
    nudge,
  };
}
