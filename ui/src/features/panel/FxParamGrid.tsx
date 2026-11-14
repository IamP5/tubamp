/**
 * The hosted plugin's own parameters, as small knobs in the editors' 68px
 * cells: up to two rows, paged when there are more than fit.
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
 * React state (guardrail §4.4). Keyed by the plugin's identifier by its
 * parent, so a different plugin opens on its first page. The knob paints itself from those values by
 * direct DOM writes, like the kit Knob, whose look (and classes) it shares.
 */
import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type RefObject,
} from "react";
import { motionValue, type MotionValue } from "motion/react";
import {
  ChevronLeftIcon,
  ChevronRightIcon,
  IconButton,
  MinusIcon,
  PlusIcon,
  cx,
  kit,
} from "../../components";
import { bridge, type FxParamInfo, type FxSlotIndex } from "../../bridge";
import f from "./fxSlot.module.css";

interface Cell {
  value: MotionValue<number>;
  /** The plugin's formatted string. */
  text: MotionValue<string>;
}

/** The editors' knob cell (ui.module.css `.cell`), so these line up with them. */
const CELL_W = 68;
const MAX_ROWS = 2;

export interface FxParamGridProps {
  slot: FxSlotIndex;
  params: readonly FxParamInfo[];
  /** The plugin's reported latency; printed under the knobs when non-zero. */
  latencySamples: number;
  /** Fade in: the plugin has just finished loading (not on opening the editor
   *  for one that was already running). */
  appear?: boolean;
}

export function FxParamGrid({ slot, params, latencySamples, appear = false }: FxParamGridProps) {
  /** Parameters with a finger on them, keyed by `param.index` — see the echo
   *  guard below. */
  const dragging = useRef<Set<number>>(new Set());
  const setDragging = useCallback((index: number, active: boolean) => {
    if (active) dragging.current.add(index);
    else dragging.current.delete(index);
  }, []);

  /* Each knob owns its own motion values and registers them here, keyed by
     `param.index`, so a structural `fxSlotChanged` (a latency change, a
     refreshed list) keeps the same instance — and the same motion values —
     under a finger that is dragging it. Knobs on other pages are unmounted;
     `latest` remembers what the plugin last said so a page turned back to
     opens on current values, not on the load-time snapshot. */
  const cells = useRef<Map<number, Cell>>(new Map());
  const latest = useRef<Map<number, { value: number; text: string }>>(new Map());

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
        const value = payload.values[i];
        const text = payload.texts[i];
        if (value !== undefined && text !== undefined) latest.current.set(param.index, { value, text });

        /* The payload is index-aligned with `params` by contract, but the lookup
           goes through `param.index` so that a contract slip degrades into a
           stale readout instead of moving a different control. */
        const cell = cells.current.get(param.index);
        if (cell === undefined) continue;

        // The readout is applied even mid-drag: a string cannot fight a pointer.
        if (text !== undefined) cell.text.set(text);

        // The position is not. The echo is a round-trip behind the finger, so
        // applying it during a drag drags the control backwards under the cursor.
        if (dragging.current.has(param.index)) continue;
        if (value !== undefined) cell.value.set(value);
      }
    });
  }, [params, slot]);

  /* How many cells fit across: measured, so the grid fills the band at any
     window size and never scrolls sideways. */
  const [box, setBox] = useState<HTMLDivElement | null>(null);
  const [columns, setColumns] = useState(0);
  useLayoutEffect(() => {
    if (!box) return;
    const measure = (): void => setColumns(Math.max(1, Math.floor(box.clientWidth / CELL_W)));
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(box);
    return () => observer.disconnect();
  }, [box]);

  const perPage = columns * MAX_ROWS;
  const pages = perPage > 0 ? Math.max(1, Math.ceil(params.length / perPage)) : 1;
  const [pageState, setPage] = useState(0);
  const page = Math.min(pageState, pages - 1);

  const visible = perPage > 0 ? params.slice(page * perPage, (page + 1) * perPage) : [];
  const rows: FxParamInfo[][] = [];
  for (let i = 0; i < visible.length; i += columns) rows.push(visible.slice(i, i + columns));

  const showBar = pages > 1 || latencySamples > 0;

  return (
    <div ref={setBox} className={f.grid}>
      <div className={cx(f.gridInner, appear && f.appear)}>
        {rows.map((row, r) => (
          <div key={r} className={f.gridRow}>
            {row.map((param) => (
              <div key={param.index} className={f.cell}>
                <FxKnob
                  slot={slot}
                  param={param}
                  latest={latest}
                  onRegister={registerCell}
                  onDragChange={setDragging}
                />
              </div>
            ))}
          </div>
        ))}
        {showBar && (
          <div className={f.bar}>
            <span className={f.barNote}>
              {latencySamples > 0 ? `Latency ${latencySamples} samples` : ""}
            </span>
            {pages > 1 && (
              <span className={f.pager}>
                <IconButton
                  size="sm"
                  aria-label="Previous parameters"
                  disabled={page === 0}
                  onClick={() => setPage(page - 1)}
                >
                  <ChevronLeftIcon />
                </IconButton>
                <span className={f.pageCount}>
                  {page + 1} / {pages}
                </span>
                <IconButton
                  size="sm"
                  aria-label="Next parameters"
                  disabled={page >= pages - 1}
                  onClick={() => setPage(page + 1)}
                >
                  <ChevronRightIcon />
                </IconButton>
              </span>
            )}
          </div>
        )}
      </div>
    </div>
  );
}

/* ──────────────────────────────── one knob ─────────────────────────────── */

/* The kit Knob's geometry at size sm (components/Knob.tsx). */
const D = 32;
const C = D / 2;
const ARC_W = 2;
const ARC_R = C - ARC_W / 2;
const DISC_R = ARC_R - ARC_W / 2 - 2 - 0.5;
const SWEEP_DEG = 270;
const START_DEG = -135;
const ARC_LEN = ARC_R * ((SWEEP_DEG * Math.PI) / 180);
const DRAG_RANGE_PX = 200;
const FINE_FACTOR = 0.2;
const STEP = 0.01;

function polar(r: number, deg: number): [number, number] {
  const a = (deg * Math.PI) / 180;
  return [C + r * Math.sin(a), C - r * Math.cos(a)];
}
const [AX, AY] = polar(ARC_R, START_DEG);
const [BX, BY] = polar(ARC_R, START_DEG + SWEEP_DEG);
const ARC_PATH = `M${AX} ${AY}A${ARC_R} ${ARC_R} 0 1 1 ${BX} ${BY}`;

const clamp01 = (v: number): number => Math.min(1, Math.max(0, v));

interface FxKnobProps {
  slot: FxSlotIndex;
  param: FxParamInfo;
  /** What the plugin last pushed, per parameter: newer than the snapshot. */
  latest: RefObject<Map<number, { value: number; text: string }>>;
  onRegister(index: number, cell: Cell | null): void;
  onDragChange(index: number, active: boolean): void;
}

/**
 * A hosted parameter as a kit-look knob: vertical drag (shift = fine), wheel
 * and the − / + steps nudge by 1% of travel, each wrapped in a gesture.
 *
 * No double-click reset and no `data-param-index`, unlike tubamp's own knobs: a
 * hosted parameter has no default we can know and no APVTS index for Logic's
 * touch-to-select to report.
 */
function FxKnob({ slot, param, latest, onRegister, onDragChange }: FxKnobProps) {
  const index = param.index;
  const rootRef = useRef<HTMLDivElement>(null);
  const dialRef = useRef<HTMLDivElement>(null);
  const pointerRef = useRef<SVGLineElement>(null);
  const arcRef = useRef<SVGPathElement>(null);
  const readoutRef = useRef<HTMLSpanElement>(null);
  const drag = useRef<{ y: number; value: number } | null>(null);
  const settle = useRef(0);

  /* Created once per mounted knob — surviving a structural update is the
     point, see the grid's comment. */
  const [cell] = useState<Cell>(() => {
    const seed = latest.current.get(param.index);
    return {
      value: motionValue(seed?.value ?? param.value),
      text: motionValue(seed?.text ?? param.text),
    };
  });

  useEffect(() => {
    onRegister(index, cell);
    return () => onRegister(index, null);
  }, [cell, index, onRegister]);

  useLayoutEffect(() => {
    const paint = (v: number): void => {
      pointerRef.current?.setAttribute("transform", `rotate(${START_DEG + v * SWEEP_DEG} ${C} ${C})`);
      arcRef.current?.setAttribute("stroke-dasharray", `${v * ARC_LEN} ${ARC_LEN * 2}`);
      dialRef.current?.setAttribute("aria-valuenow", v.toFixed(3));
    };
    const label = (t: string): void => {
      if (readoutRef.current) readoutRef.current.textContent = t;
      const unit = param.label && !t.endsWith(param.label) ? ` ${param.label}` : "";
      dialRef.current?.setAttribute("aria-valuetext", `${t}${unit}`);
    };
    paint(cell.value.get());
    label(cell.text.get());
    const offValue = cell.value.on("change", paint);
    const offText = cell.text.on("change", label);
    return () => {
      offValue();
      offText();
    };
  }, [cell, param.label]);

  useEffect(() => () => window.clearTimeout(settle.current), []);

  const write = useCallback(
    (next: number) => {
      const v = clamp01(next);
      cell.value.set(v);
      void bridge.fxSetParam(slot, index, v);
    },
    [cell, index, slot],
  );

  /** One step as its own gesture. The echo guard stays up a moment after, so a
   *  push already in flight cannot snap the knob back. */
  const nudge = useCallback(
    (delta: number) => {
      onDragChange(index, true);
      void bridge.fxBeginGesture(slot, index);
      write(cell.value.get() + delta);
      void bridge.fxEndGesture(slot, index);
      window.clearTimeout(settle.current);
      settle.current = window.setTimeout(() => {
        if (!drag.current) onDragChange(index, false);
      }, 150);
    },
    [cell, index, onDragChange, slot, write],
  );

  useEffect(() => {
    const el = dialRef.current;
    if (!el) return;
    const onWheel = (e: WheelEvent): void => {
      if (e.deltaY === 0 || Math.abs(e.deltaX) > Math.abs(e.deltaY)) return;
      e.preventDefault();
      e.stopPropagation();
      nudge((e.deltaY > 0 ? -STEP : STEP) * (e.shiftKey ? FINE_FACTOR : 1));
    };
    el.addEventListener("wheel", onWheel, { passive: false });
    return () => el.removeEventListener("wheel", onWheel);
  }, [nudge]);

  const setActive = (active: boolean): void => {
    if (active) rootRef.current?.setAttribute("data-active", "true");
    else rootRef.current?.removeAttribute("data-active");
  };

  return (
    <div ref={rootRef} className={cx(kit.knob, kit.knobSm, f.knob)} title={param.name}>
      <div
        ref={dialRef}
        className={kit.knobDial}
        style={{ width: D, height: D }}
        role="slider"
        aria-label={param.name}
        aria-valuemin={0}
        aria-valuemax={1}
        onPointerDown={(e) => {
          if (e.button !== 0) return;
          e.currentTarget.setPointerCapture(e.pointerId);
          drag.current = { y: e.clientY, value: cell.value.get() };
          setActive(true);
          onDragChange(index, true);
          void bridge.fxBeginGesture(slot, index);
        }}
        onPointerMove={(e) => {
          const start = drag.current;
          if (!start) return;
          const factor = e.shiftKey ? FINE_FACTOR : 1;
          write(start.value + ((start.y - e.clientY) / DRAG_RANGE_PX) * factor);
        }}
        onPointerUp={(e) => endDrag(e.currentTarget, e.pointerId)}
        onPointerCancel={(e) => endDrag(e.currentTarget, e.pointerId)}
      >
        <svg className={kit.knobSvg} width={D} height={D} viewBox={`0 0 ${D} ${D}`}>
          <path className={kit.knobTrack} d={ARC_PATH} strokeWidth={ARC_W} />
          <path
            ref={arcRef}
            className={kit.knobArc}
            d={ARC_PATH}
            strokeWidth={ARC_W}
            strokeDasharray={`0 ${ARC_LEN * 2}`}
          />
          <circle className={kit.knobDisc} cx={C} cy={C} r={DISC_R} />
          <line
            ref={pointerRef}
            className={kit.knobPointer}
            x1={C}
            y1={C - DISC_R * 0.35}
            x2={C}
            y2={C - (DISC_R - 2.5)}
          />
        </svg>
      </div>
      <span className={kit.knobLabel}>{param.name}</span>
      <span className={kit.knobValueRow}>
        <button
          type="button"
          className={cx(kit.knobStep, kit.knobStepDec)}
          aria-label={`Decrease ${param.name}`}
          tabIndex={-1}
          onClick={() => nudge(-STEP)}
        >
          <MinusIcon size={12} strokeWidth={2} />
        </button>
        <span ref={readoutRef} className={cx(kit.knobReadout, f.readout)} />
        <button
          type="button"
          className={cx(kit.knobStep, kit.knobStepInc)}
          aria-label={`Increase ${param.name}`}
          tabIndex={-1}
          onClick={() => nudge(STEP)}
        >
          <PlusIcon size={12} strokeWidth={2} />
        </button>
      </span>
    </div>
  );

  function endDrag(target: HTMLDivElement, pointerId: number): void {
    if (!drag.current) return;
    drag.current = null;
    if (target.hasPointerCapture(pointerId)) target.releasePointerCapture(pointerId);
    setActive(false);
    onDragChange(index, false);
    void bridge.fxEndGesture(slot, index);
  }
}
