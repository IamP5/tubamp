/**
 * The hosted plugin's own parameters.
 *
 * These are NOT tubamp parameters and cannot be: a JUCE relay needs a static id
 * at editor-construction time, while a hosted plugin's list is unknown until it
 * loads and changes again when it is replaced. So the list arrives inside
 * `FxSlotState.params`, edits go out as `fxSetParam` calls wrapped in a gesture,
 * and live values come back on the `fxParamValues` event — always normalised
 * 0..1, with the plugin's own string as the only trustworthy readout.
 *
 * That event fires at meter rate for every parameter at once, so it is consumed
 * exactly like `meters`: straight off the bridge into motion values, never into
 * React state (guardrail §4.4). A 60-parameter plugin would otherwise re-render
 * sixty rows thirty times a second.
 */
import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type PointerEvent as ReactPointerEvent,
} from "react";
import { motion, motionValue, useTransform, type MotionValue } from "motion/react";
import { bridge, type FxParamInfo, type FxSlotIndex } from "../../bridge";
import s from "./panel.module.css";

interface Cell {
  value: MotionValue<number>;
  /** The plugin's formatted string — rendered as a motion child, so a new
   *  readout is a text-node write rather than a render. */
  text: MotionValue<string>;
}

export interface FxParamGridProps {
  slot: FxSlotIndex;
  params: readonly FxParamInfo[];
}

export function FxParamGrid({ slot, params }: FxParamGridProps) {
  /** Parameters with a finger on them, keyed by `param.index` — see the echo
   *  guard below. */
  const dragging = useRef<Set<number>>(new Set());
  const setDragging = useCallback((index: number, active: boolean) => {
    if (active) dragging.current.add(index);
    else dragging.current.delete(index);
  }, []);

  /* Each row owns its own motion values and registers them here. Ownership sits
     with the row rather than this grid so that a structural `fxSlotChanged`
     (a latency change, a refreshed parameter list) cannot disturb a control the
     user is dragging: the rows are keyed by `param.index`, so React keeps the
     same instance — and therefore the same motion values — across those updates,
     and only a genuinely different parameter list remounts them. */
  const cells = useRef<Map<number, Cell>>(new Map());

  const registerCell = useCallback((index: number, cell: Cell | null) => {
    if (cell === null) cells.current.delete(index);
    else cells.current.set(index, cell);
  }, []);

  useEffect(() => {
    return bridge.on("fxParamValues", (payload) => {
      if (payload.slot !== slot) return;

      for (let i = 0; i < params.length; i += 1) {
        const param = params[i];
        if (param === undefined) continue;

        /* The payload is index-aligned with `params` by contract — the C++ emits
           a placeholder rather than skipping a null parameter — but the lookup
           goes through `param.index` so that a contract slip degrades into a
           stale readout instead of moving a different control. */
        const cell = cells.current.get(param.index);
        if (cell === undefined) continue;

        // The readout is applied even mid-drag: a string cannot fight a pointer,
        // and freezing it would stall the number exactly while it is being read.
        const text = payload.texts[i];
        if (text !== undefined) cell.text.set(text);

        // The position is not. The echo is a round-trip behind the finger, so
        // applying it during a drag drags the control backwards under the cursor.
        if (dragging.current.has(param.index)) continue;
        const value = payload.values[i];
        if (value !== undefined) cell.value.set(value);
      }
    });
  }, [params, slot]);

  return (
    <div className={s.fxGrid}>
      {params.map((param) => (
        <FxParamRow
          key={param.index}
          slot={slot}
          param={param}
          onRegister={registerCell}
          onDragChange={setDragging}
        />
      ))}
    </div>
  );
}

/* ──────────────────────────────── one row ──────────────────────────────── */

interface FxParamRowProps {
  slot: FxSlotIndex;
  param: FxParamInfo;
  onRegister(index: number, cell: Cell | null): void;
  onDragChange(index: number, active: boolean): void;
}

/**
 * Name, a 0..1 track and the plugin's readout.
 *
 * No double-click-to-default and no `data-param-index`, unlike tubamp's own
 * controls: a hosted parameter has no default we can know and no APVTS index for
 * Logic's touch-to-select to report.
 */
function FxParamRow({ slot, param, onRegister, onDragChange }: FxParamRowProps) {
  const index = param.index;
  const trackRef = useRef<HTMLDivElement>(null);
  const dragging = useRef(false);

  /* Created once per mounted row and seeded from the snapshot that mounted it.
     Surviving a structural update is the point — see the grid's comment. */
  const [cell] = useState<Cell>(() => ({
    value: motionValue(param.value),
    text: motionValue(param.text),
  }));

  useEffect(() => {
    onRegister(index, cell);
    return () => onRegister(index, null);
  }, [cell, index, onRegister]);

  const width = useTransform(cell.value, (v) => `${(v * 100).toFixed(2)}%`);

  const writeFromClientX = useCallback(
    (clientX: number) => {
      const el = trackRef.current;
      if (!el) return;
      const rect = el.getBoundingClientRect();
      if (rect.width <= 0) return;
      const next = Math.min(1, Math.max(0, (clientX - rect.left) / rect.width));
      cell.value.set(next);
      void bridge.fxSetParam(slot, index, next);
    },
    [cell, index, slot],
  );

  const onPointerDown = useCallback(
    (event: ReactPointerEvent<HTMLDivElement>) => {
      if (event.button !== 0) return;
      event.currentTarget.setPointerCapture(event.pointerId);
      dragging.current = true;
      onDragChange(index, true);
      void bridge.fxBeginGesture(slot, index);
      writeFromClientX(event.clientX);
    },
    [index, onDragChange, slot, writeFromClientX],
  );

  const onPointerMove = useCallback(
    (event: ReactPointerEvent<HTMLDivElement>) => {
      if (!dragging.current) return;
      writeFromClientX(event.clientX);
    },
    [writeFromClientX],
  );

  const endDrag = useCallback(
    (event: ReactPointerEvent<HTMLDivElement>) => {
      if (!dragging.current) return;
      dragging.current = false;
      onDragChange(index, false);
      if (event.currentTarget.hasPointerCapture(event.pointerId)) {
        event.currentTarget.releasePointerCapture(event.pointerId);
      }
      void bridge.fxEndGesture(slot, index);
    },
    [index, onDragChange, slot],
  );

  return (
    <div className={s.fxParamRow}>
      <span className={s.fxParamName} title={param.name}>
        {param.name}
      </span>
      <div
        ref={trackRef}
        className={s.fxParamTrack}
        role="slider"
        aria-label={param.name}
        aria-valuemin={0}
        aria-valuemax={1}
        /* Seeded from the load-time snapshot and then left alone: the live value
           lives in a motion value precisely so that 30 Hz of updates never
           re-render these rows, and reflecting it here would undo that. It gives
           a screen reader the right starting point and the units. */
        aria-valuenow={param.value}
        aria-valuetext={param.text ? `${param.text} ${param.label}`.trim() : undefined}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
      >
        <motion.div className={s.sliderFill} style={{ width }} />
      </div>
      <motion.span className={s.fxParamReadout}>{cell.text}</motion.span>
    </div>
  );
}
