/**
 * Status bar (36px) — ui/DESIGN.md §Layout.
 *
 *   Input [meter] 0.0 dB     48 kHz · 1.9 ms     0.0 dB [meter] Output
 *
 * Input trim and output level are compact value fields (drag horizontally,
 * double-click resets, minus / plus step on hover), not knobs. Meters are motion-value
 * driven (`useMeters`, shared rAF), so nothing here re-renders per meter frame.
 */
import { useCallback, useRef, type PointerEvent as ReactPointerEvent } from "react";
import s from "./footer.module.css";
import { useStore } from "../../store";
import { Meter, MinusIcon, PlusIcon, Tooltip, cx } from "../../components";
import { useMeters, useSliderParam } from "../../hooks";
import { isMockBridge, type SliderParamId } from "../../bridge";
import { engineStatus, paramValue } from "../../theme/words";

/**
 * `ModelInfo.latencySamples` is the processor's reported latency, not a
 * model-only figure: it is the NAM engine's latency when an enabled amp block
 * holds a model and 0 otherwise, so with no model there is nothing to print.
 */
export function Footer() {
  const model = useStore((st) => st.model);
  const meters = useMeters();
  const status = model ? engineStatus(model.sampleRateHz, model.latencySamples) : "";

  return (
    <div className={s.bar}>
      <div className={s.side}>
        <span className={s.label}>Input</span>
        <Meter
          channel={meters.input}
          orientation="horizontal"
          className={s.meter}
          aria-label="Input level"
        />
        <ValueField id="input_trim" label="In Trim" />
      </div>

      <div className={s.centre}>
        {status && (
          <Tooltip label="Model sample rate and latency">
            <span className={s.status}>{status}</span>
          </Tooltip>
        )}
        {isMockBridge && <span className={s.status}>mock bridge</span>}
      </div>

      <div className={cx(s.side, s.sideEnd)}>
        <ValueField id="output_level" label="Out Level" />
        <Meter
          channel={meters.output}
          orientation="horizontal"
          className={s.meter}
          aria-label="Output level"
        />
        <span className={s.label}>Output</span>
      </div>
    </div>
  );
}

/** Normalised travel per pixel of horizontal drag: the full range in 360px. */
const DRAG_PER_PX = 1 / 360;

/** One step-button click, in parameter units: half a dB for levels. */
function stepOf(unit: string, interval: number): number {
  return unit === "dB" ? 0.5 : Math.max(interval, 0.01);
}

/**
 * A level as a number you can drag. The slider semantics (role, aria-label,
 * value range) sit on the number; `data-param-index` on the cell lets Logic's
 * touch-to-select find it.
 */
function ValueField({ id, label }: { id: SliderParamId; label: string }) {
  const p = useSliderParam(id);
  const drag = useRef<{ pointerId: number; x: number; from: number } | null>(null);
  const text = paramValue(id, p.scaled, p.properties);
  const step = stepOf(p.label, p.properties.interval);

  const onPointerDown = useCallback(
    (e: ReactPointerEvent<HTMLSpanElement>) => {
      if (e.button !== 0) return;
      e.preventDefault();
      drag.current = { pointerId: e.pointerId, x: e.clientX, from: p.value };
      e.currentTarget.setPointerCapture(e.pointerId);
      p.beginGesture();
    },
    [p],
  );

  const onPointerMove = useCallback(
    (e: ReactPointerEvent<HTMLSpanElement>) => {
      const d = drag.current;
      if (!d || e.pointerId !== d.pointerId) return;
      const fine = e.shiftKey ? 0.2 : 1;
      p.setNormalised(d.from + (e.clientX - d.x) * DRAG_PER_PX * fine);
    },
    [p],
  );

  const endDrag = useCallback(
    (e: ReactPointerEvent<HTMLSpanElement>) => {
      const d = drag.current;
      if (!d || e.pointerId !== d.pointerId) return;
      drag.current = null;
      p.endGesture();
    },
    [p],
  );

  const stepBy = (dir: number): void => {
    p.beginGesture();
    p.setScaled(p.scaled + dir * step);
    p.endGesture();
  };

  return (
    <div className={s.field} data-param-index={p.parameterIndex} title={p.name}>
      <button
        type="button"
        className={s.step}
        aria-label={`Decrease ${label}`}
        tabIndex={-1}
        onClick={() => stepBy(-1)}
      >
        <MinusIcon size={12} />
      </button>
      <span
        className={s.value}
        role="slider"
        aria-label={label}
        aria-valuemin={p.properties.start}
        aria-valuemax={p.properties.end}
        aria-valuenow={p.scaled}
        aria-valuetext={text}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
        onLostPointerCapture={endDrag}
        onDoubleClick={p.reset}
      >
        {text}
      </span>
      <button
        type="button"
        className={s.step}
        aria-label={`Increase ${label}`}
        tabIndex={-1}
        onClick={() => stepBy(1)}
      >
        <PlusIcon size={12} />
      </button>
    </div>
  );
}
