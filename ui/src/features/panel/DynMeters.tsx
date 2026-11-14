/**
 * Pieces the gate and compressor editors share.
 *
 * `GrMeter`: the gain-reduction column the gate and compressor displays end with
 * (docs/FX-ENGINES.md §7.4): a 10px meter-ink bar that grows down from 0 dB,
 * its own small dB scale beside it, a 1px peak-hold tick, and optionally the
 * deepest the reduction may go (the gate's range). Fed by `useDynMeters`; the
 * bar moves through `onFrame` with direct attribute writes, never a React
 * render.
 */
import { useEffect, useRef } from "react";
import type { MotionValue } from "motion/react";
import { onFrame } from "../../hooks";
import { cx } from "../../components";
import { Plot, PlotSplit, clamp, plot as p } from "./display";
import d from "./dyn.module.css";

/** Vertical inset of every dynamics scale, so a 0 dB label and a handle at the
 *  top are not cut by the display's edge. */
export const DYN_PAD_Y = 10;

const BAR_W = 8;
const BAR_RIGHT = 12;
const PEAK_HOLD_S = 1;
const PEAK_FALL_DB_PER_S = 20;

export function GrMeter({
  value,
  scaleDb,
  ticks,
  floorDb,
  label = "Gain reduction",
}: {
  /** Smoothed gain reduction, dB >= 0. */
  value: MotionValue<number>;
  /** The bottom of the bar, dB of reduction. */
  scaleDb: number;
  /** Reduction depths labelled on the scale, dB >= 0. */
  ticks: readonly number[];
  /** The most the effect can reduce right now (the gate's range), dB >= 0;
   *  drawn as a reference across the bar when it is on the scale. */
  floorDb?: number;
  label?: string;
}) {
  const bar = useRef<SVGRectElement | null>(null);
  const peak = useRef<SVGRectElement | null>(null);

  useEffect(() => {
    let held = 0;
    let heldFor = 0;
    let lastBar = "";
    let lastPeak = "";
    let lastEl: SVGRectElement | null = null;
    return onFrame((dt) => {
      const el = bar.current;
      if (!el) return;
      if (el !== lastEl) {
        lastEl = el;
        lastBar = lastPeak = "";
      }
      const top = Number(el.getAttribute("y"));
      const span = Number(el.dataset.span);
      const gr = Math.max(0, value.get());
      if (gr >= held) {
        held = gr;
        heldFor = 0;
      } else if ((heldFor += dt) > PEAK_HOLD_S) {
        held = Math.max(gr, held - PEAK_FALL_DB_PER_S * dt);
      }
      const height = (clamp(gr / scaleDb, 0, 1) * span).toFixed(1);
      if (height !== lastBar) el.setAttribute("height", (lastBar = height));
      const tick = peak.current;
      if (tick) {
        const y = held < 0.05 ? "" : (top + clamp(held / scaleDb, 0, 1) * span - 1).toFixed(1);
        if (y !== lastPeak) {
          lastPeak = y;
          tick.setAttribute("y", y || String(top));
          tick.style.opacity = y ? "1" : "0";
        }
      }
    });
  }, [value, scaleDb]);

  return (
    <>
      <PlotSplit />
      <div className={d.grColumn}>
        <Plot aria-label={label}>
          {({ w, h }) => {
            const top = DYN_PAD_Y;
            const span = h - 2 * DYN_PAD_Y;
            const x = w - BAR_RIGHT - BAR_W;
            const yOf = (db: number): number => top + clamp(db / scaleDb, 0, 1) * span;
            return (
              <>
                {ticks.map((db) => {
                  const y = Math.round(yOf(db)) + 0.5;
                  return (
                    <g key={db}>
                      <line className={p.grid} x1={x - 4} x2={x} y1={y} y2={y} />
                      <text className={cx(p.tick, d.scaleText)} x={x - 7} y={y + 4}>
                        {db === 0 ? "0" : `−${db}`}
                      </text>
                    </g>
                  );
                })}
                <rect className={d.track} x={x} y={top} width={BAR_W} height={span} rx={2} />
                <rect ref={bar} className={p.meter} x={x} y={top} width={BAR_W} height={0} data-span={span} />
                <rect ref={peak} className={p.meter} x={x} y={top} width={BAR_W} height={1} opacity={0} />
                {floorDb !== undefined && floorDb < scaleDb && (
                  <line
                    className={d.floor}
                    x1={x - 2}
                    x2={x + BAR_W + 2}
                    y1={Math.round(yOf(floorDb)) + 0.5}
                    y2={Math.round(yOf(floorDb)) + 0.5}
                  />
                )}
              </>
            );
          }}
        </Plot>
      </div>
    </>
  );
}
