/**
 * The connector layer: one SVG path per gap between two lane elements, with flow
 * chevrons drifting along it (motion-design.md §2.3), plus the split container's
 * frame.
 *
 * Geometry is read straight off the nodes' motion values on a frame callback
 * that is only scheduled when something actually moves, so connectors stay glued
 * to the cards through drag, reflow and settle without a single
 * `getBoundingClientRect` or React render per frame. `d` is written with
 * `setAttribute`, never through React (guardrail §4.6b). The container frame is
 * drawn here for the same reason: its left and right edges are SPLIT's and MIX's
 * motion values, so it follows their springs instead of snapping a re-rendered
 * `width` around them.
 *
 * Four shapes. Within a row (or a lane track) the wire is a straight `H` — a
 * slight bezier while a card is lifted off the line, which is also exactly the
 * shape the split's fan-out and the mixer's converge want. Across a row wrap it
 * is a dedicated route — out to the right of the row end, down through the row
 * gap, back in to the left of the next row's first card — because a mid-x bezier
 * between a large x1 and a small x2 sweeps backwards across every card in the row
 * it is leaving. An EMPTY lane's wire bows out of SPLIT, along its own track and
 * back into MIX: that bow is the only thing standing for the lane, so it has to
 * be visibly in it.
 */
import { useCallback, useLayoutEffect, useRef } from "react";
import { cancelFrame, frame } from "motion/react";
import { isBlockId, type BlockId } from "../../bridge/types";
import { BLOCK_ACCENT } from "../../theme/blocks";
import { FLOW_ARROW_PATH_D, type Box, type LaneRegion, type Link } from "./layout";
import type { NodeMotionRegistry } from "./nodes";
import s from "./board.module.css";

export interface ConnectorsProps {
  links: readonly Link[];
  /** The split container's frame, when the board has one. */
  region: LaneRegion | null;
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

/** How far an empty lane's bow runs out of SPLIT before it reaches its track. */
const BOW_LEAD = 30;

/** Air between the container frame and the cards inside it. */
const REGION_PAD_X = 20;
const REGION_PAD_Y = 14;
const REGION_R = 18;

function straightPath(x1: number, y1: number, x2: number, y2: number): string {
  if (Math.abs(y1 - y2) < 0.5) return `M${x1} ${y1}H${x2}`;
  // A card lifted by a drag, and the split's fan-out / the mixer's converge: a
  // gentle S between two near-horizontal anchors.
  const mid = (x1 + x2) / 2;
  return `M${x1} ${y1}C${mid} ${y1} ${mid} ${y2} ${x2} ${y2}`;
}

/**
 * Out past the end of the row, down into the gutter, back along it, then down
 * into the start of the next row — four rounded right angles. A single curve
 * between the two anchors would sweep backwards across every card in the row it
 * is leaving; this route only ever runs through empty board.
 */
function wrapPath(
  x1: number,
  y1: number,
  x2: number,
  y2: number,
  mid: number,
): string {
  const out = x1 + WRAP_LEAD;
  const back = x2 - WRAP_LEAD;
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

/** An empty lane: out of SPLIT, along the lane's own track, back into MIX. */
function bowPath(
  x1: number,
  y1: number,
  x2: number,
  y2: number,
  track: number,
): string {
  const out = Math.min(x1 + BOW_LEAD, (x1 + x2) / 2);
  const back = Math.max(x2 - BOW_LEAD, (x1 + x2) / 2);
  return (
    `M${x1} ${y1}C${out} ${y1} ${out} ${track} ${out + 1} ${track}` +
    `H${back - 1}` +
    `C${back} ${track} ${back} ${y2} ${x2} ${y2}`
  );
}

export function Connectors({
  links,
  region,
  box,
  nodes,
  selected,
  bypassed,
  reduced,
}: ConnectorsProps) {
  const paths = useRef<(SVGPathElement | null)[]>([]);
  const anchors = useRef<(SVGGElement | null)[]>([]);
  const frameEl = useRef<SVGRectElement | null>(null);

  const draw = useCallback(() => {
    for (let i = 0; i < links.length; i += 1) {
      const link = links[i];
      if (!link) continue;
      const a = nodes.get(link.from.id);
      const b = nodes.get(link.to.id);
      const x1 = a.x.get() + link.from.w;
      const y1 = a.y.get();
      const x2 = b.x.get();
      const y2 = b.y.get();
      // Branch on the LINK, never on Δy: a dragged card is off its row's centre
      // line and its neighbours' wires must stay straight, and a fan-out is a
      // legitimate Δy that is not a wrap.
      const bowed = !link.wrap && link.via !== undefined;

      paths.current[i]?.setAttribute(
        "d",
        link.wrap
          ? wrapPath(x1, y1, x2, y2, link.via ?? (y1 + y2) / 2)
          : bowed
            ? bowPath(x1, y1, x2, y2, link.via!)
            : straightPath(x1, y1, x2, y2),
      );
      const anchor = anchors.current[i];
      if (anchor) {
        // On a wrap (and on a lane bow) the chevrons run out of the row end and
        // fade, which is the only part of the route that is horizontal — the
        // flow keyframes only translate x. Within a row they cross the whole gap.
        const lead = link.wrap || bowed;
        anchor.setAttribute(
          "transform",
          lead ? `translate(${x1} ${y1})` : `translate(${x1} ${(y1 + y2) / 2})`,
        );
        anchor.style.setProperty(
          "--flow-len",
          `${lead ? WRAP_FLOW_LEN : Math.max(x2 - x1, 0)}px`,
        );
      }
    }

    const rect = frameEl.current;
    if (rect && region) {
      const left = nodes.get(region.head).x.get() - REGION_PAD_X;
      const right =
        nodes.get(region.tail).x.get() + region.tailW + REGION_PAD_X;
      rect.setAttribute("x", `${left}`);
      rect.setAttribute("width", `${Math.max(right - left, 0)}`);
    }
  }, [links, nodes, region]);

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
    const watch = (id: Parameters<NodeMotionRegistry["get"]>[0]): void => {
      const node = nodes.get(id);
      unsubscribe.push(
        node.x.on("change", schedule),
        node.y.on("change", schedule),
      );
    };
    for (const link of links) {
      watch(link.from.id);
      watch(link.to.id);
    }
    if (region) {
      watch(region.head);
      watch(region.tail);
    }
    draw();

    return () => {
      for (const off of unsubscribe) off();
      if (queued) cancelFrame(run);
    };
  }, [draw, links, nodes, region]);

  const segments = links.map((link, i) => {
    const feedsSelection = selected !== null && link.to.id === selected;
    const dimmed =
      (isBlockId(link.from.id) && bypassed.has(link.from.id)) ||
      (isBlockId(link.to.id) && bypassed.has(link.to.id));
    return { key: link.key, i, feedsSelection, dimmed };
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
      {/* Behind every wire: the split container. x / width are written by the
          frame callback above; y / height are static, because rows do not move. */}
      {region && (
        <rect
          ref={frameEl}
          className={s.laneRegion}
          x={0}
          width={0}
          y={region.y - REGION_PAD_Y}
          height={region.h + REGION_PAD_Y * 2}
          rx={REGION_R}
        />
      )}

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
