/**
 * The connector layer: one SVG path per gap between consecutive lane elements,
 * with flow chevrons drifting along it (motion-design.md §2.3).
 *
 * Geometry is read straight off the nodes' motion values on a frame callback
 * that is only scheduled when something actually moves, so connectors stay glued
 * to the cards through drag, reflow and settle without a single
 * `getBoundingClientRect` or React render per frame. `d` is written with
 * `setAttribute`, never through React (guardrail §4.6b).
 *
 * Two shapes. Within a row the wire is a straight `H` (a slight bezier while a
 * card is lifted off the line). Across a row wrap it is a dedicated route — out
 * to the right of the row end, down through the row gap, back in to the left of
 * the next row's first card — because a mid-x bezier between a large x1 and a
 * small x2 sweeps backwards across every card in the row it is leaving.
 */
import { useCallback, useLayoutEffect, useRef } from "react";
import { cancelFrame, frame } from "motion/react";
import { isBlockId, type BlockId } from "../../bridge/types";
import { BLOCK_ACCENT } from "../../theme/blocks";
import { FLOW_ARROW_PATH_D, type Box, type Slot } from "./layout";
import type { NodeMotionRegistry } from "./nodes";
import s from "./board.module.css";

export interface ConnectorsProps {
  slots: readonly Slot[];
  /** SVG box in lane units (see layout.connectorBox). */
  box: Box;
  nodes: NodeMotionRegistry;
  selected: BlockId | null;
  /** Blocks whose `*_on` parameter is off — their segments dim. */
  bypassed: ReadonlySet<BlockId>;
  reduced: boolean;
}

/** How far a wrap runs straight out of the row end / into the next row start. */
const WRAP_LEAD = 34;
/** Corner radius of the wrap's four turns. Fits the row gap: the return lane
 *  runs down its middle, ROW_GAP / 2 clear of both rows. */
const WRAP_R = 18;
/** Chevron travel on a wrap: the lead-out, the part that is still horizontal. */
const WRAP_FLOW_LEN = WRAP_LEAD;

function straightPath(x1: number, y1: number, x2: number, y2: number): string {
  if (Math.abs(y1 - y2) < 0.5) return `M${x1} ${y1}H${x2}`;
  // A card lifted by a drag: a gentle S between two near-horizontal anchors.
  const mid = (x1 + x2) / 2;
  return `M${x1} ${y1}C${mid} ${y1} ${mid} ${y2} ${x2} ${y2}`;
}

/**
 * Out past the end of the row, down into the gutter, back along it, then down
 * into the start of the next row — four rounded right angles. A single curve
 * between the two anchors would sweep backwards across every card in the row it
 * is leaving; this route only ever runs through empty board.
 */
function wrapPath(x1: number, y1: number, x2: number, y2: number): string {
  const out = x1 + WRAP_LEAD;
  const back = x2 - WRAP_LEAD;
  const mid = (y1 + y2) / 2;
  const r = Math.min(WRAP_R, Math.abs(out - back) / 2, Math.abs(mid - y1));
  return (
    `M${x1} ${y1}H${out - r}` +
    `Q${out} ${y1} ${out} ${y1 + r}` +
    `V${mid - r}` +
    `Q${out} ${mid} ${out - r} ${mid}` +
    `H${back + r}` +
    `Q${back} ${mid} ${back} ${mid + r}` +
    `V${y2 - r}` +
    `Q${back} ${y2} ${back + r} ${y2}` +
    `H${x2}`
  );
}

export function Connectors({
  slots,
  box,
  nodes,
  selected,
  bypassed,
  reduced,
}: ConnectorsProps) {
  const paths = useRef<(SVGPathElement | null)[]>([]);
  const anchors = useRef<(SVGGElement | null)[]>([]);

  const draw = useCallback(() => {
    for (let i = 0; i < slots.length - 1; i += 1) {
      const from = slots[i];
      const to = slots[i + 1];
      if (!from || !to) continue;
      const a = nodes.get(from.id);
      const b = nodes.get(to.id);
      const x1 = a.x.get() + from.w;
      const y1 = a.y.get();
      const x2 = b.x.get();
      const y2 = b.y.get();
      // Branch on the ROW, never on Δy: a dragged card is off its row's centre
      // line and its neighbours' wires must stay straight.
      const wraps = from.row !== to.row;

      paths.current[i]?.setAttribute(
        "d",
        wraps ? wrapPath(x1, y1, x2, y2) : straightPath(x1, y1, x2, y2),
      );
      const anchor = anchors.current[i];
      if (anchor) {
        // On a wrap the chevrons run out of the row end and fade, which is the
        // only part of the route that is horizontal — the flow keyframes only
        // translate x. Within a row they cross the whole gap.
        anchor.setAttribute(
          "transform",
          wraps ? `translate(${x1} ${y1})` : `translate(${x1} ${(y1 + y2) / 2})`,
        );
        anchor.style.setProperty(
          "--flow-len",
          `${wraps ? WRAP_FLOW_LEN : Math.max(x2 - x1, 0)}px`,
        );
      }
    }
  }, [nodes, slots]);

  useLayoutEffect(() => {
    let queued = false;
    const run = (): void => {
      queued = false;
      draw();
    };
    const schedule = (): void => {
      if (queued) return;
      queued = true;
      frame.render(run);
    };

    const unsubscribe: (() => void)[] = [];
    for (const slot of slots) {
      const node = nodes.get(slot.id);
      unsubscribe.push(node.x.on("change", schedule), node.y.on("change", schedule));
    }
    draw();

    return () => {
      for (const off of unsubscribe) off();
      if (queued) cancelFrame(run);
    };
  }, [draw, nodes, slots]);

  const segments = slots.slice(0, -1).map((from, i) => {
    const to = slots[i + 1];
    const feedsSelection = selected !== null && to?.id === selected;
    const dimmed =
      (isBlockId(from.id) && bypassed.has(from.id)) ||
      (to !== undefined && isBlockId(to.id) && bypassed.has(to.id));
    return { key: `${from.id}->${to?.id ?? "end"}`, i, feedsSelection, dimmed };
  });

  return (
    <svg
      className={s.connectors}
      width={box.w}
      height={box.h}
      style={{ left: box.x, top: box.y }}
      viewBox={`${box.x} ${box.y} ${box.w} ${box.h}`}
      aria-hidden="true"
    >
      {segments.map(({ key, i, feedsSelection, dimmed }) => (
        <g key={key} opacity={dimmed ? 0.35 : 1} className={s.segment}>
          <path
            ref={(el) => {
              paths.current[i] = el;
            }}
            className={s.wire}
            style={
              feedsSelection && selected
                ? { stroke: BLOCK_ACCENT[selected], strokeOpacity: 0.85 }
                : undefined
            }
          />
          {!reduced && (
            <g
              ref={(el) => {
                anchors.current[i] = el;
              }}
            >
              <g className={s.flow}>
                <path className={s.chevron} d={FLOW_ARROW_PATH_D} />
              </g>
              <g className={`${s.flow} ${s.flowTrail}`}>
                <path className={s.chevron} d={FLOW_ARROW_PATH_D} />
              </g>
            </g>
          )}
        </g>
      ))}
    </svg>
  );
}
