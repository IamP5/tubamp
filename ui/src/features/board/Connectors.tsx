/**
 * The wire layer: one SVG path per gap between two lane elements, plus the
 * split region's frame. Wires are quiet — one neutral 1.5px stroke with round
 * joins, no arrows, no per-block or per-lane colour, unchanged by selection or
 * bypass (ui/DESIGN.md).
 *
 * They are live, though: each wire is drawn twice, and the second, brighter
 * stroke's opacity follows the signal — the input meter along the chain, the
 * output meter on the last stretch into Out. Nothing travels and nothing
 * loops; the wires lift while you play and settle when you stop.
 *
 * Geometry is read straight off the nodes' motion values on a frame callback
 * that is only scheduled when something actually moves, so wires stay glued to
 * the tiles through drag, reflow and settle without a single
 * `getBoundingClientRect` or React render per frame. `d` is written with
 * `setAttribute`, never through React. The region frame is drawn here for the
 * same reason: its left and right edges are the Split and Mixer tiles' motion
 * values, so it follows their springs instead of snapping a re-rendered
 * `width` around them.
 *
 * Shapes. Within a row (or a lane track) the wire is a straight `H` — a slight
 * bezier while a tile is lifted off the line, which is also exactly the shape
 * the split's fan-out and the mixer's converge want. Across a row wrap it is a
 * dedicated route — out to the right of the row end, down through the row gap,
 * back in to the left of the next row's first tile, four rounded corners —
 * because a mid-x bezier between a large x1 and a small x2 sweeps backwards
 * across every tile in the row it is leaving. Lane wires normally end in the
 * lane's own add slot; only when there is nothing left to add (no slot) does an
 * EMPTY lane's wire bow out of Split, along its own track and back into Mixer.
 */
import { useCallback, useEffect, useLayoutEffect, useRef } from "react";
import { cancelFrame, frame } from "motion/react";
import { useMeters } from "../../hooks";
import {
  REGION_PAD_X,
  REGION_R,
  type Box,
  type LaneRegion,
  type Link,
} from "./layout";
import type { NodeMotionRegistry } from "./nodes";
import s from "./board.module.css";

export interface ConnectorsProps {
  links: readonly Link[];
  /** The split container's frame, when the board has one. */
  region: LaneRegion | null;
  /** SVG box in lane units (see layout.connectorBox). */
  box: Box;
  nodes: NodeMotionRegistry;
}

/** How far a wrap runs straight out of the row end / into the next row start.
 *  Less than FIRST_CARD_X - TERMINAL_W, so the return stays left of row 1's
 *  first tile. */
const WRAP_LEAD = 20;
/** Corner radius of the wrap's four turns. Fits the row gap: the return lane
 *  runs down its middle, ROW_GAP / 2 clear of both rows. */
const WRAP_R = 10;

/** Meter position (0..1 over −60…+6 dB) where a wire starts to lift, and how
 *  bright it gets at full scale. Below about −40 dB a wire is at rest. */
const SIGNAL_FLOOR = 0.3;
const SIGNAL_MAX_OPACITY = 0.5;

function signalOpacity(level: number): string {
  const t = Math.min(1, Math.max(0, (level - SIGNAL_FLOOR) / (1 - SIGNAL_FLOOR)));
  return (t * SIGNAL_MAX_OPACITY).toFixed(3);
}

/** The last stretch — into the add tile and Out — carries the output. */
function carriesOutput(link: Link): boolean {
  return link.to.id === "out" || link.to.id === "add" || link.from.id === "add";
}

/** How far an empty lane's bow runs out of Split before it reaches its track. */
const BOW_LEAD = 20;

function straightPath(x1: number, y1: number, x2: number, y2: number): string {
  if (Math.abs(y1 - y2) < 0.5) return `M${x1} ${y1}H${x2}`;
  // A tile lifted by a drag, and the split's fan-out / the mixer's converge: a
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

export function Connectors({ links, region, box, nodes }: ConnectorsProps) {
  const paths = useRef<(SVGPathElement | null)[]>([]);
  const signals = useRef<(SVGPathElement | null)[]>([]);
  const frameEl = useRef<SVGRectElement | null>(null);
  const meters = useMeters();

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

      const d = link.wrap
        ? wrapPath(x1, y1, x2, y2, link.via ?? (y1 + y2) / 2)
        : bowed
          ? bowPath(x1, y1, x2, y2, link.via!)
          : straightPath(x1, y1, x2, y2);
      paths.current[i]?.setAttribute("d", d);
      signals.current[i]?.setAttribute("d", d);
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

  /* The signal strokes: opacity written straight to the DOM whenever a meter's
     smoothed level moves, which is at most once a frame and not at all in
     silence. */
  useEffect(() => {
    const paint = (output: boolean, level: number): void => {
      const opacity = signalOpacity(level);
      for (let i = 0; i < links.length; i += 1) {
        const link = links[i];
        if (link && carriesOutput(link) === output) {
          const el = signals.current[i];
          if (el) el.style.opacity = opacity;
        }
      }
    };
    paint(false, meters.input.level.get());
    paint(true, meters.output.level.get());
    const offIn = meters.input.level.on("change", (v) => paint(false, v));
    const offOut = meters.output.level.on("change", (v) => paint(true, v));
    return () => {
      offIn();
      offOut();
    };
  }, [links, meters]);

  return (
    <svg
      className={s.connectors}
      width={box.w}
      height={box.h}
      style={{ left: box.x, top: box.y }}
      viewBox={`${box.x} ${box.y} ${box.w} ${box.h}`}
      aria-hidden="true"
    >
      {/* Behind every wire: the split region's frame. x / width are written by the
          frame callback above; y / height are static, because rows do not move. */}
      {region && (
        <rect
          ref={frameEl}
          className={s.laneRegion}
          x={0}
          width={0}
          y={region.y}
          height={region.h}
          rx={REGION_R}
        />
      )}

      {links.map((link, i) => (
        <path
          key={link.key}
          ref={(el) => {
            paths.current[i] = el;
          }}
          className={s.wire}
        />
      ))}
      {links.map((link, i) => (
        <path
          key={link.key}
          ref={(el) => {
            signals.current[i] = el;
          }}
          className={s.wireSignal}
        />
      ))}
    </svg>
  );
}
