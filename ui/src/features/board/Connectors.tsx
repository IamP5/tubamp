/**
 * The connector layer: one SVG path per gap between consecutive lane elements,
 * with flow chevrons drifting along it (motion-design.md §2.3).
 *
 * Geometry is read straight off the nodes' motion values on a frame callback
 * that is only scheduled when something actually moves, so connectors stay glued
 * to the cards through drag, reflow and settle without a single
 * `getBoundingClientRect` or React render per frame. `d` is written with
 * `setAttribute`, never through React (guardrail §4.6b).
 */
import { useCallback, useLayoutEffect, useRef } from "react";
import { cancelFrame, frame } from "motion/react";
import { isBlockId, type BlockId } from "../../bridge/types";
import { BLOCK_ACCENT } from "../../theme/blocks";
import { CONNECTOR_SVG, FLOW_ARROW_PATH_D, type Slot } from "./layout";
import type { NodeMotionRegistry } from "./nodes";
import s from "./board.module.css";

export interface ConnectorsProps {
  slots: readonly Slot[];
  nodes: NodeMotionRegistry;
  selected: BlockId | null;
  /** Blocks whose `*_on` parameter is off — their segments dim. */
  bypassed: ReadonlySet<BlockId>;
  reduced: boolean;
}

function segmentPath(x1: number, y1: number, x2: number, y2: number): string {
  if (Math.abs(y1 - y2) < 0.5) return `M${x1} ${y1}H${x2}`;
  const mid = (x1 + x2) / 2;
  return `M${x1} ${y1}C${mid} ${y1} ${mid} ${y2} ${x2} ${y2}`;
}

export function Connectors({
  slots,
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
      const a = nodes.get(from.id);
      const b = nodes.get(to.id);
      const x1 = a.x.get() + from.w;
      const y1 = a.y.get();
      const x2 = b.x.get();
      const y2 = b.y.get();

      paths.current[i]?.setAttribute("d", segmentPath(x1, y1, x2, y2));
      const anchor = anchors.current[i];
      if (anchor) {
        anchor.setAttribute("transform", `translate(${x1} ${(y1 + y2) / 2})`);
        // Inherited by both chevron runners inside — the travel distance is the
        // gap width, so the drift reads at a constant speed whatever the gap.
        anchor.style.setProperty("--flow-len", `${Math.max(x2 - x1, 0)}px`);
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
    const feedsSelection = selected !== null && to.id === selected;
    const dimmed =
      (isBlockId(from.id) && bypassed.has(from.id)) ||
      (isBlockId(to.id) && bypassed.has(to.id));
    return { key: `${from.id}->${to.id}`, i, feedsSelection, dimmed };
  });

  return (
    <svg
      className={s.connectors}
      width={CONNECTOR_SVG.w}
      height={CONNECTOR_SVG.h}
      style={{ left: CONNECTOR_SVG.left, top: CONNECTOR_SVG.top }}
      aria-hidden="true"
    >
      <g transform={`translate(${-CONNECTOR_SVG.left} ${-CONNECTOR_SVG.top})`}>
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
      </g>
    </svg>
  );
}
