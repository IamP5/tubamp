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
 * Width. Where the processor's width map says the signal is stereo (left and
 * right differ), the wire is two thin rails instead of one stroke, left above
 * right, each lifted by its own output channel — so a ping-pong breathes on
 * one rail, then the other. Rails start or merge at the exit of the tile that
 * turns the width; the tooltip there says why (index.tsx `TurnPort`).
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
import { cancelFrame, frame, type MotionValue } from "motion/react";
import { useMeters } from "../../hooks";
import type { Width } from "../../bridge/types";
import {
  REGION_PAD_X,
  REGION_R,
  type Box,
  type LaneRegion,
  type Link,
} from "./layout";
import type { NodeMotionRegistry } from "./nodes";
import { RAIL_OFFSET, bowPath, straightPath, wrapPath } from "./wires";
import s from "./board.module.css";

export interface ConnectorsProps {
  links: readonly Link[];
  /** Each link's width (`wires.linkWidths`), or null to draw every wire as one
   *  stroke. */
  widths: readonly Width[] | null;
  /** The split container's frame, when the board has one. */
  region: LaneRegion | null;
  /** SVG box in lane units (see layout.connectorBox). */
  box: Box;
  nodes: NodeMotionRegistry;
  /** No signal strokes under reduced motion; the wires and rails stay. */
  reduced: boolean;
}

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

/** Which meter lifts a stroke: a mono wire's one stroke follows the input along
 *  the chain and the output on the last stretch; a stereo wire's rails follow
 *  the output's left and right channels. */
type Lift = "in" | "out" | "left" | "right";

/** Rail offsets of a mono wire (its centre line) and a stereo one (left above,
 *  right below). */
const MONO_RAILS = [0] as const;
const STEREO_RAILS = [RAIL_OFFSET, -RAIL_OFFSET] as const;

function railsOf(width: Width | undefined): readonly number[] {
  return width === "stereo" ? STEREO_RAILS : MONO_RAILS;
}

function liftOf(link: Link, stereo: boolean, rail: number): Lift {
  if (stereo) return rail === 0 ? "left" : "right";
  return carriesOutput(link) ? "out" : "in";
}

export function Connectors({ links, widths, region, box, nodes, reduced }: ConnectorsProps) {
  const wires = useRef<(SVGPathElement | null)[][]>([]);
  const signals = useRef<(SVGPathElement | null)[][]>([]);
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
      const rails = railsOf(widths?.[i]);
      for (let k = 0; k < rails.length; k += 1) {
        const o = rails[k]!;
        const d = link.wrap
          ? wrapPath(x1, y1, x2, y2, link.via ?? (y1 + y2) / 2, o)
          : bowed
            ? bowPath(x1, y1, x2, y2, link.via!, o)
            : straightPath(x1, y1, x2, y2, o);
        wires.current[i]?.[k]?.setAttribute("d", d);
        signals.current[i]?.[k]?.setAttribute("d", d);
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
  }, [links, nodes, region, widths]);

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
     silence. Which meter lifts each stroke is resolved once per links/widths
     change, so a frame only walks its own strokes. Under reduced motion the
     strokes are not drawn at all. */
  useEffect(() => {
    if (reduced) return;
    const byLift: Record<Lift, SVGPathElement[]> = { in: [], out: [], left: [], right: [] };
    for (let i = 0; i < links.length; i += 1) {
      const link = links[i];
      if (!link) continue;
      const stereo = widths?.[i] === "stereo";
      const rails = railsOf(widths?.[i]);
      for (let k = 0; k < rails.length; k += 1) {
        const el = signals.current[i]?.[k];
        if (el) byLift[liftOf(link, stereo, k)].push(el);
      }
    }
    const sources: Record<Lift, MotionValue<number>> = {
      in: meters.input.level,
      out: meters.output.level,
      left: meters.outputL.level,
      right: meters.outputR.level,
    };
    const offs = (Object.keys(sources) as Lift[]).map((lift) => {
      const paint = (level: number): void => {
        const opacity = signalOpacity(level);
        for (const el of byLift[lift]) el.style.opacity = opacity;
      };
      paint(sources[lift].get());
      return sources[lift].on("change", paint);
    });
    return () => {
      for (const off of offs) off();
    };
  }, [links, meters, reduced, widths]);

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

      {links.map((link, i) => {
        const stereo = widths?.[i] === "stereo";
        return railsOf(widths?.[i]).map((_, k) => (
          <path
            key={`${link.key}#${stereo ? k : "c"}`}
            ref={(el) => {
              (wires.current[i] ??= [])[k] = el;
            }}
            className={stereo ? s.rail : s.wire}
          />
        ));
      })}
      {!reduced &&
        links.map((link, i) => {
          const stereo = widths?.[i] === "stereo";
          return railsOf(widths?.[i]).map((_, k) => (
            <path
              key={`${link.key}#${stereo ? k : "c"}`}
              ref={(el) => {
                (signals.current[i] ??= [])[k] = el;
              }}
              className={stereo ? s.railSignal : s.wireSignal}
            />
          ));
        })}
    </svg>
  );
}
