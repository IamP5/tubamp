/**
 * What the wire layer draws, as pure functions: each route's path, for the
 * centre line of a mono wire or for one rail of a stereo wire, and how the
 * processor's `WidthView` maps onto the board's links and tiles.
 *
 * A rail is the route shifted by `o` to the left of the direction of travel,
 * so the left channel (o > 0) runs above the right one on every straight run
 * and the two never cross through a wrap. The wrap's corners are offset
 * exactly (the outer corner's radius grows by `o`, the inner one's shrinks);
 * a cubic is offset by moving its control polygon's legs (Tiller-Hanson),
 * which is exact enough at a couple of pixels.
 */
import type { Width, WidthStep, WidthView, WidthWhy } from "../../bridge/types";
import type { LaneSide } from "../../chain/structure";

/** Half the distance between a stereo wire's two rails. */
export const RAIL_OFFSET = 1.75;

/** How far a wrap runs straight out of the row end / into the next row start.
 *  Less than FIRST_CARD_X - TERMINAL_W, so the return stays left of row 1's
 *  first tile. */
const WRAP_LEAD = 20;
/** Corner radius of the wrap's four turns. Fits the row gap: the return lane
 *  runs down its middle, ROW_GAP / 2 clear of both rows. */
const WRAP_R = 10;
/** How far an empty lane's bow runs out of Split before it reaches its track. */
const BOW_LEAD = 20;
/** Where the bow's curve meets its straight run, past the curve's corner. More
 *  than RAIL_OFFSET, so an offset rail's corner never folds back on itself. */
const BOW_JOIN = 4;

export function straightPath(
  x1: number,
  y1: number,
  x2: number,
  y2: number,
  o = 0,
): string {
  if (Math.abs(y1 - y2) < 0.5) return `M${x1} ${y1 - o}H${x2}`;
  // A tile lifted by a drag, and the split's fan-out / the mixer's converge: a
  // gentle S between two near-horizontal anchors.
  const mid = (x1 + x2) / 2 + Math.sign(y2 - y1) * o;
  return `M${x1} ${y1 - o}C${mid} ${y1 - o} ${mid} ${y2 - o} ${x2} ${y2 - o}`;
}

/**
 * Out past the end of the row, down into the gutter, back along it, then down
 * into the start of the next row — four rounded right angles. A single curve
 * between the two anchors would sweep backwards across every card in the row it
 * is leaving; this route only ever runs through empty board.
 */
export function wrapPath(
  x1: number,
  y1: number,
  x2: number,
  y2: number,
  mid: number,
  o = 0,
): string {
  const out = x1 + WRAP_LEAD;
  const back = x2 - WRAP_LEAD;
  const r = Math.min(WRAP_R, Math.abs(out - back) / 2, Math.abs(mid - y1));
  return (
    `M${x1} ${y1 - o}H${out - r}` +
    `Q${out + o} ${y1 - o} ${out + o} ${y1 + r}` +
    `V${mid - r}` +
    `Q${out + o} ${mid + o} ${out - r} ${mid + o}` +
    `H${back + r}` +
    `Q${back + o} ${mid + o} ${back + o} ${mid + r}` +
    `V${y2 - r}` +
    `Q${back + o} ${y2 - o} ${back + r} ${y2 - o}` +
    `H${x2}`
  );
}

/** An empty lane: out of SPLIT, along the lane's own track, back into MIX. */
export function bowPath(
  x1: number,
  y1: number,
  x2: number,
  y2: number,
  track: number,
  o = 0,
): string {
  const out = Math.min(x1 + BOW_LEAD, (x1 + x2) / 2);
  const back = Math.max(x2 - BOW_LEAD, (x1 + x2) / 2);
  const t = track - o;
  const outX = out + Math.sign(track - y1) * o;
  const backX = back + Math.sign(y2 - track) * o;
  return (
    `M${x1} ${y1 - o}C${outX} ${y1 - o} ${outX} ${t} ${out + BOW_JOIN} ${t}` +
    `H${back - BOW_JOIN}` +
    `C${backX} ${t} ${backX} ${y2 - o} ${x2} ${y2 - o}`
  );
}

/** The parts of a board link the width mapping reads. */
export interface LinkEnds {
  from: { id: string };
  to: { id: string };
  lane?: LaneSide;
}

/**
 * Each link's width, in link order: the width of what leaves the node the link
 * starts at. In takes `input`, the wire into Out takes `output`, Split's lane B
 * fan-out takes `outB`. Furniture without a step (a lane's add slot, the tail
 * add tile) passes on what arrived at it. Keyed by token, not position, so a
 * drag preview reads the moved block's own width. Null without a view: the
 * board then draws exactly as it did before width existed.
 */
export function linkWidths(
  view: WidthView | undefined,
  links: readonly LinkEnds[],
): Width[] | null {
  if (!view) return null;
  const steps = new Map(view.steps.map((step) => [step.block, step]));
  const into = new Map<string, Width>();
  return links.map((link) => {
    const from = link.from.id;
    const step = steps.get(from);
    const width =
      link.to.id === "out"
        ? view.output
        : from === "in"
          ? view.input
          : step
            ? (link.lane === "b" && from === "split" ? step.outB : undefined) ?? step.out
            : into.get(from) ?? "mono";
    into.set(link.to.id, width);
    return width;
  });
}

const WIDEN_WHY: Partial<Record<WidthWhy, string>> = {
  pingPong: "ping-pong repeats",
  dualRatio: "dual delay times",
  delayMod: "modulated repeats",
  diffuse: "diffused repeats",
  reverb: "reverb width",
  chorus: "wide chorus",
  tremolo: "stereo tremolo",
  stereoIr: "stereo IR",
  lanes: "lanes panned apart",
  plugin: "plugin output",
};

const FOLD_WHY: Partial<Record<WidthWhy, string>> = {
  amp: "the amp sums left and right",
  amp2: "the amp sums left and right",
  pluginMono: "plugin is mono only",
};

/** The tooltip at a tile's exit where the width turns; null where it does not. */
export function turnText(step: WidthStep): string | null {
  const [lead, whys] =
    step.turn === "widens"
      ? ["Stereo from here", WIDEN_WHY]
      : step.turn === "folds"
        ? ["Mono from here", FOLD_WHY]
        : [null, null];
  if (!lead) return null;
  const why = step.why ? whys[step.why] : undefined;
  return why ? `${lead} · ${why}` : lead;
}

/** What a mono-out instance whose chain would be stereo tells the user: the
 *  popover at Out, and (joined) the one-time toast. */
export const MONO_INSTANCE_TEXT = [
  "This track is mono, so stereo effects play in mono.",
  "Insert tubamp as Mono → Stereo to hear them in stereo.", // ds-allow: Logic's name for the layout
] as const;
