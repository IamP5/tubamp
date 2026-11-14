/**
 * Board geometry — rows of blocks, and the split container inside one of them.
 *
 * The chain is laid out analytically rather than by DOM flow: every node (IN
 * terminal, block cards, [+] node, OUT terminal) is absolutely positioned by a
 * pair of `x`/`y` motion values, so reflow/drag/settle are pure transform
 * animations and connector geometry can be derived from the very same numbers —
 * no `getBoundingClientRect` in any per-frame path, and no Motion `layout`
 * projection inside the zoomed stage (which would double-apply the stage scale).
 *
 * Coordinate space: "lane units". x = 0 is the left edge of the IN terminal,
 * y = 0 is the centre line of the FIRST row; rows below it are stacked by their
 * own heights (see `rowGeometry`) — a plain row is CARD_H tall, so a board with
 * no split region keeps the old constant `ROW_STRIDE` spacing exactly.
 * `y` names a node's centre (every node carries `marginTop: -h / 2`), which is
 * what makes a straight connector between two same-row nodes a single `H`.
 *
 * The chain is one flat order; `rows` (from the store's `rowsFor`, then through
 * `laneRows` here) is the partition that says where it wraps. Rows are
 * left-aligned on a SHARED column grid: card k of every row sits at
 * `cardSlotX(k)`, so the cards line up vertically and one x→index rule serves
 * every row. Row 0 additionally carries IN at x = 0; the last row carries [+]
 * and OUT after its last card.
 *
 * THE SPLIT CONTAINER (docs/SPLIT.md §5). When the order carries a valid
 * structure, the whole region `split … mix` is kept inside exactly ONE row
 * (`laneRows`) — it never straddles a wrap — but it SHARES that row with any
 * serial blocks around it, so a short chain like IN → AMP → [split] → OUT reads
 * left to right on one line. Inside its row the region is laid out on the same
 * column grid as everything else: SPLIT on the row's centre line, lane A's cards
 * on an upper track and lane B's on a lower track (each track half the row's
 * height), each lane closed by its own small [+] slot, MIX on the centre line
 * after the longer lane, then the row's serial blocks carry on. A row holding
 * the region is LANE_ROW_H tall instead of CARD_H, which is all the stage's
 * fit/pan box needs to know — it measures slots.
 *
 * Auto rows (`autoRows`) wrap a structured chain by COLUMNS rather than tokens:
 * the region counts as its real width and is never split, every other block is
 * one column, ROW_WRAP columns to a row. A structureless chain wraps exactly as
 * the store's `rowsFor` always has.
 *
 * `lane2` has no slot: which lane a block is in is its POSITION relative to that
 * token in the flat order, so the token is furniture and never renders.
 */
import type { BlockId } from "../../bridge/types";
import {
  findStructure,
  laneSpans,
  type LaneSide,
  type LaneSpans,
} from "../../chain/structure";
import { ROW_WRAP, rowsFor } from "../../store";

export type { LaneSide };

/** Block card face, per docs/REACT-UI.md §UX structure. */
export const CARD_W = 112;
export const CARD_H = 80;

/** IN / OUT terminal badges and the [+] node share one square footprint. */
export const TERMINAL_W = 60;
export const TERMINAL_H = 60;
export const ADD_W = 60;
export const ADD_H = 60;

/** Gap between every pair of lane elements (the connector runs live here). */
export const GAP = 44;

/** Distance between two adjacent card slots. */
export const SLOT_STRIDE = CARD_W + GAP;

/** x of the first card slot in EVERY row: IN terminal + one gap. */
export const FIRST_CARD_X = TERMINAL_W + GAP;

/**
 * Vertical air between two rows. Wide enough for the wrap connector to loop
 * back through it without touching either row, and for a card's note pill.
 */
export const ROW_GAP = 56;

/** Distance between two adjacent row centre lines — plain rows only; a row's
 *  real spacing comes from `rowGeometry`, which is identical to this whenever
 *  the board has no split container. */
export const ROW_STRIDE = CARD_H + ROW_GAP;

/** Vertical air between the two lane tracks of a split container. Narrower than
 *  ROW_GAP: the tracks belong together and no wrap route runs between them. */
export const LANE_TRACK_GAP = 24;

/** How far each lane track's centre line sits off the container's centre line. */
export const LANE_OFFSET = (CARD_H + LANE_TRACK_GAP) / 2;

/** Height of a split container: two card-tall tracks and the air between them. */
export const LANE_ROW_H = CARD_H * 2 + LANE_TRACK_GAP;

/** First lane column, counted from the SPLIT tile's own column. */
export const LANE_FIRST_COLUMN = 1;

/** The lane-scoped [+] slot that closes each lane: small, dashed, centred in
 *  the column after the lane's last card. */
export const LANE_ADD_W = 36;
export const LANE_ADD_H = 36;

/** Air between the split container's frame and the cards inside it. The lane
 *  badges sit ON the frame's top and bottom edges, so the board needs these
 *  too, not only the connector layer that draws the frame. */
export const REGION_PAD_X = 20;
export const REGION_PAD_Y = 14;
export const REGION_R = 18;

/**
 * Height of the band the param panel covers while a block is selected — the JS
 * twin of `--panel-band` under `[data-panel-open]` (theme/tokens.css, where it
 * is `--dock-h + --dock-gap`). The panel is an overlay,
 * so the board paints full-bleed underneath it; this value only feeds the
 * stage's framing decisions (fit, pan clamp, ensure-visible) and is 0 whenever
 * nothing is selected.
 */
export const PANEL_BAND = 260;

/** Vertical lift applied to a card while it is being dragged. */
export const DRAG_LIFT = 6;

/** Pointer travel (screen px) before a press turns into a reorder drag. */
export const DRAG_THRESHOLD_PX = 5;

/**
 * Slack added around the slot boxes when the content bounding box is measured:
 * terminals carry a caption below them and cards a note pill, and neither is
 * part of a slot's own rectangle.
 */
export const CONTENT_PAD_Y = 28;

/** Slack on each side of the connector SVG, so a card dragged off the end of a
 *  row still has wire under it. */
const CONNECTOR_PAD_X = 400;
const CONNECTOR_PAD_Y = 160;

/**
 * `icons::makeFlowArrow()` from the deleted BlockIcons.h, scaled to the same 7px
 * box the native painter used and re-centred on the origin so it can be
 * translated along a connector.
 */
export const FLOW_ARROW_PATH_D = "M-1.82 -2.38L1.82 0L-1.82 2.38";

/** "lane-a" / "lane-b" are the two lane [+] slots. */
export type NodeId = "in" | "out" | "add" | "lane-a" | "lane-b" | BlockId;

export function laneAddId(side: LaneSide): "lane-a" | "lane-b" {
  return side === "a" ? "lane-a" : "lane-b";
}

/** An axis-aligned box in lane units. `y` is the TOP edge, not a centre line. */
export interface Box {
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface Slot {
  id: NodeId;
  /** Left edge in lane units. */
  x: number;
  /** Centre line in lane units — `y = 0` is row 0. */
  y: number;
  w: number;
  h: number;
  /** Which row the node belongs to; connectors branch on this, never on Δy
   *  (a dragged card is lifted off its row's centre line and must not turn its
   *  neighbours' wires into wrap routes). */
  row: number;
  /** Which lane track the node sits on inside a split container. Absent for
   *  everything on a row's own centre line — including SPLIT and MIX, which are
   *  the container's ends, not lane members. */
  lane?: LaneSide;
}

/** One row's vertical geometry and its slice of the flat order. */
export interface RowGeom {
  /** Centre line in lane units. */
  y: number;
  /** Content height: CARD_H, or LANE_ROW_H for a split container. */
  h: number;
  /** Flat index of the row's first token. */
  start: number;
  /** Tokens in the row — `lane2` included, because `rows` partitions the ORDER
   *  and not the set of rendered cards. */
  count: number;
  /** This row holds the split region (`split … mix`, whole). */
  lanes: boolean;
  /** Column of the SPLIT tile within the row; -1 when `lanes` is false. */
  regionColumn: number;
  /** Lane columns between SPLIT and MIX (each lane's cards plus its [+] slot,
   *  whichever lane is longer); 0 when `lanes` is false. */
  laneColumns: number;
}

/**
 * One connector: a gap between two nodes, plus everything the painter and the
 * insert-[+] need to know about it. Replaces "consecutive slots in the array",
 * which stopped being able to describe the board the moment it forked.
 */
export interface Link {
  key: string;
  from: Slot;
  to: Slot;
  /** A row wrap: the route goes out, down the gutter and back. */
  wrap: boolean;
  /** y the route is bent through: the gutter's middle on a wrap, the lane
   *  track's centre line on an empty lane's bypass wire. */
  via?: number;
  /** Flat index a block inserted in this gap lands at; -1 for the gaps into
   *  [+]/OUT, which mean "append" and belong to the [+] node. */
  index: number;
  /** Row an insert here grows. */
  row: number;
  /** The lane this wire belongs to, for its tint. */
  lane?: LaneSide;
}

/**
 * The split container's frame. Its left/right edges follow SPLIT's and MIX's
 * motion values (the connector layer redraws it on the same frame callback as
 * the wires), so it stays glued to them through drag and reflow; the vertical
 * numbers are static, because rows do not animate.
 */
export interface LaneRegion {
  head: NodeId;
  tail: NodeId;
  tailW: number;
  /** Top edge and height in lane units. */
  y: number;
  h: number;
  /** Centre lines of the two lane tracks. */
  trackA: number;
  trackB: number;
  /** Left edge of the first lane column — where the lane badges sit. */
  laneX: number;
  /** Row the region lives in (lane inserts grow it). */
  row: number;
}

export interface BoardGeometry {
  slots: Slot[];
  links: Link[];
  rows: RowGeom[];
  region: LaneRegion | null;
}

/* There is deliberately no `rowY(row)` any more: a row's centre line depends on
   whether the rows above it are plain or split containers, so it is only ever
   correct to read it out of `rowGeometry`. */

/** Resting x of card `column` of any row (independent of the row's contents). */
export function cardSlotX(column: number): number {
  return FIRST_CARD_X + column * SLOT_STRIDE;
}

/** First index of row `row`; past the end it is the chain length (= append). */
export function rowStart(rows: readonly number[], row: number): number {
  let index = 0;
  for (let r = 0; r < rows.length && r < row; r += 1) index += rows[r] ?? 0;
  return index;
}

/**
 * The partition with the split region kept inside exactly one row.
 *
 * `rows` is layout-only and tolerant, and a partition restored from C++ (or
 * written by an older build) knows nothing about structure — so a region can
 * arrive straddling two rows. The board drops every row break that falls INSIDE
 * the region and keeps every other break the user made; the region may share
 * its row with serial blocks on either side. Idempotent, and the identity
 * function on a structureless order.
 *
 * Everything that needs to know where a block sits — geometry, drag, connectors,
 * the commit — reads the partition THROUGH this (see `displayRows`).
 */
export function laneRows(
  order: readonly BlockId[],
  rows: readonly number[],
): number[] {
  const structure = findStructure(order);
  if (!structure.valid || structure.splitAt < 0 || rows.length === 0)
    return [...rows];

  const next: number[] = [];
  let prev = 0;
  let at = 0;
  for (const length of rows) {
    at += length;
    // A break after token `at - 1`: inside the region when split <= at-1 < mix.
    if (at > structure.splitAt && at <= structure.mixAt) continue;
    if (at > prev && at <= order.length) next.push(at - prev);
    prev = Math.max(prev, at);
  }
  if (prev < order.length) next.push(order.length - prev);
  return next;
}

/** Lane columns a region needs: the longer lane's cards, plus its [+] slot when
 *  the board shows one — and never fewer than one, or an empty fork would
 *  collapse into the gap between its two tiles. */
export function laneColumnsFor(spans: LaneSpans, laneAdd: boolean): number {
  const plus = laneAdd ? 1 : 0;
  return Math.max(spans.a.count + plus, spans.b.count + plus, 1);
}

/**
 * The automatic partition — what the board draws while the store holds `[]`.
 *
 * Structureless: exactly the store's `rowsFor` (ROW_WRAP blocks a row), so an
 * ordinary chain lays out precisely as it always has. Structured: a greedy wrap
 * by COLUMNS, where the region is one unbreakable unit as wide as it really is
 * (SPLIT + lane columns + MIX) and every other block is one column — so a short
 * chain stays on one row and only wraps when the row would outgrow ROW_WRAP.
 */
export function autoRows(
  order: readonly BlockId[],
  laneAdd: boolean,
): number[] {
  const structure = findStructure(order);
  const spans = laneSpans(structure);
  if (!spans) return rowsFor(order, []);

  const regionTokens = structure.mixAt - structure.splitAt + 1;
  const regionCols = 2 + laneColumnsFor(spans, laneAdd);
  const rows: number[] = [];
  let tokens = 0;
  let cols = 0;
  let i = 0;
  while (i < order.length) {
    const region = i === structure.splitAt;
    const width = region ? regionCols : 1;
    const count = region ? regionTokens : 1;
    if (cols > 0 && cols + width > ROW_WRAP) {
      rows.push(tokens);
      tokens = 0;
      cols = 0;
    }
    tokens += count;
    cols += width;
    i += count;
  }
  if (tokens > 0) rows.push(tokens);
  return rows;
}

/**
 * The rows the board draws: the published partition (made region-safe) when
 * there is one, the automatic wrap when the store holds `[]`. The single source
 * of the partition — board, drag and commit all read it from here.
 */
export function displayRows(
  order: readonly BlockId[],
  chainRows: readonly number[],
  laneAdd: boolean,
): number[] {
  return chainRows.length > 0
    ? laneRows(order, chainRows)
    : autoRows(order, laneAdd);
}

/**
 * Per-row vertical geometry. A row carries the split container only when it
 * holds the whole region — a caller that skipped `laneRows` gets plain rows
 * rather than a half-drawn container.
 */
export function rowGeometry(
  order: readonly BlockId[],
  rows: readonly number[],
  laneAdd = false,
): RowGeom[] {
  const structure = findStructure(order);
  const spans = laneSpans(structure);
  const geom: RowGeom[] = [];
  const rowCount = Math.max(rows.length, 1);
  let start = 0;

  for (let row = 0; row < rowCount; row += 1) {
    const count = rows[row] ?? 0;
    const lanes =
      spans !== null &&
      structure.splitAt >= start &&
      structure.mixAt <= start + count - 1;
    const h = lanes ? LANE_ROW_H : CARD_H;
    const previous = geom[row - 1];
    const y =
      previous === undefined
        ? 0
        : previous.y + previous.h / 2 + ROW_GAP + h / 2;
    geom.push({
      y,
      h,
      start,
      count,
      lanes,
      // Every token before SPLIT in its row is a serial block — the region is
      // the only multi-token thing on the board — so its column is its offset.
      regionColumn: lanes ? structure.splitAt - start : -1,
      laneColumns: lanes && spans ? laneColumnsFor(spans, laneAdd) : 0,
    });
    start += count;
  }
  return geom;
}

/** Top / bottom edges of a row's content band. */
function rowTop(row: RowGeom): number {
  return row.y - row.h / 2;
}

function rowBottom(row: RowGeom): number {
  return row.y + row.h / 2;
}

/**
 * IN — cards… — [+]? — OUT, wrapped into `rows`, with the split region drawn as
 * a container, plus the connector graph over the result.
 *
 * `rows` must partition `order` exactly (the store's `rowsFor` guarantees it) and
 * should have been through `laneRows`; an empty chain still lays out one row, so
 * IN/[+]/OUT have somewhere to sit.
 */
export function boardGeometry(
  order: readonly BlockId[],
  rows: readonly number[],
  showAdd: boolean,
): BoardGeometry {
  const geom = rowGeometry(order, rows, showAdd);
  const structure = findStructure(order);
  const spans = laneSpans(structure);
  const slots: Slot[] = [];
  let region: LaneRegion | null = null;

  for (let row = 0; row < geom.length; row += 1) {
    const g = geom[row]!;
    const y = g.y;
    if (row === 0)
      slots.push({ id: "in", x: 0, y, w: TERMINAL_W, h: TERMINAL_H, row });

    let column = 0;
    const end = g.start + g.count;
    let i = g.start;

    while (i < end) {
      const id = order[i];
      if (id === undefined) break;

      if (g.lanes && spans && i === structure.splitAt) {
        const head = column;
        slots.push({ id: "split", x: cardSlotX(head), y, w: CARD_W, h: CARD_H, row });

        const trackA = y - LANE_OFFSET;
        const trackB = y + LANE_OFFSET;
        const lane = (span: LaneSpans["a"], at: number, side: LaneSide): void => {
          for (let k = 0; k < span.count; k += 1) {
            const card = order[span.start + k];
            if (card === undefined) break;
            slots.push({
              id: card,
              x: cardSlotX(head + LANE_FIRST_COLUMN + k),
              y: at,
              w: CARD_W,
              h: CARD_H,
              row,
              lane: side,
            });
          }
          // The lane's own [+]: centred in the column after its last card, so
          // an empty lane reads as an intentional slot, not a bare wire.
          if (showAdd)
            slots.push({
              id: laneAddId(side),
              x:
                cardSlotX(head + LANE_FIRST_COLUMN + span.count) +
                (CARD_W - LANE_ADD_W) / 2,
              y: at,
              w: LANE_ADD_W,
              h: LANE_ADD_H,
              row,
              lane: side,
            });
        };
        lane(spans.a, trackA, "a");
        lane(spans.b, trackB, "b");

        const mixColumn = head + LANE_FIRST_COLUMN + g.laneColumns;
        slots.push({ id: "mix", x: cardSlotX(mixColumn), y, w: CARD_W, h: CARD_H, row });
        region = {
          head: "split",
          tail: "mix",
          tailW: CARD_W,
          y: rowTop(g),
          h: g.h,
          trackA,
          trackB,
          laneX: cardSlotX(head + LANE_FIRST_COLUMN),
          row,
        };
        column = mixColumn + 1;
        i = structure.mixAt + 1;
        continue;
      }

      // Never a card: a lane2 outside a container is a structure the board
      // could not draw, and it is about to be flattened by the sanitizer.
      if (id !== "lane2") {
        slots.push({ id, x: cardSlotX(column), y, w: CARD_W, h: CARD_H, row });
        column += 1;
      }
      i += 1;
    }

    if (row === geom.length - 1) {
      let x = cardSlotX(column);
      if (showAdd) {
        slots.push({ id: "add", x, y, w: ADD_W, h: ADD_H, row });
        x += ADD_W + GAP;
      }
      slots.push({ id: "out", x, y, w: TERMINAL_W, h: TERMINAL_H, row });
    }
  }

  return { slots, links: connectorLinks(order, geom, slots), rows: geom, region };
}

/** Slots only — the drag hook's view of the board. */
export function computeSlots(
  order: readonly BlockId[],
  rows: readonly number[],
  showAdd: boolean,
): Slot[] {
  return boardGeometry(order, rows, showAdd).slots;
}

/**
 * The connector graph: the spine (everything on a row's centre line, in visual
 * order) minus the SPLIT→MIX gap, which fans out into the two lane tracks and
 * converges again. Each lane runs SPLIT → its cards → its [+] slot → MIX; the
 * gaps into and out of that slot belong to the slot (index -1, no insert dot),
 * exactly like the gaps into the tail [+] and OUT on the main path. With no
 * [+] slots (nothing left to add) an empty lane still gets its own wire, bowed
 * through the track it stands for.
 */
function connectorLinks(
  order: readonly BlockId[],
  geom: readonly RowGeom[],
  slots: readonly Slot[],
): Link[] {
  const spine = slots.filter((slot) => slot.lane === undefined);
  const lanes: Record<LaneSide, Slot[]> = {
    a: slots.filter((slot) => slot.lane === "a"),
    b: slots.filter((slot) => slot.lane === "b"),
  };
  const structure = findStructure(order);
  const spans = laneSpans(structure);
  const links: Link[] = [];

  const isAppend = (slot: Slot): boolean =>
    slot.id === "add" ||
    slot.id === "out" ||
    slot.id === "lane-a" ||
    slot.id === "lane-b";

  /** Where a block dropped in the gap before `slot` lands. */
  const indexOf = (slot: Slot): number =>
    isAppend(slot) ? -1 : order.indexOf(slot.id as BlockId);

  const gutter = (from: Slot, to: Slot): number => {
    const above = geom[from.row];
    const below = geom[to.row];
    if (!above || !below) return (from.y + to.y) / 2;
    return (rowBottom(above) + rowTop(below)) / 2;
  };

  for (let i = 0; i < spine.length - 1; i += 1) {
    const from = spine[i]!;
    const to = spine[i + 1]!;

    if (from.id === "split" && to.id === "mix" && spans) {
      const track: Record<LaneSide, number> = {
        a: from.y - LANE_OFFSET,
        b: from.y + LANE_OFFSET,
      };
      for (const side of ["a", "b"] as const) {
        const nodes = lanes[side];
        const span = spans[side];
        const head = nodes[0];
        const tail = nodes[nodes.length - 1];

        if (!head || !tail) {
          links.push({
            key: `split->mix@${side}`,
            from,
            to,
            wrap: false,
            via: track[side],
            index: span.start,
            row: from.row,
            lane: side,
          });
          continue;
        }

        links.push({
          key: `split->${head.id}`,
          from,
          to: head,
          wrap: false,
          index: isAppend(head) ? -1 : span.start,
          row: from.row,
          lane: side,
        });
        for (let c = 0; c < nodes.length - 1; c += 1) {
          const a = nodes[c]!;
          const b = nodes[c + 1]!;
          links.push({
            key: `${a.id}->${b.id}`,
            from: a,
            to: b,
            wrap: false,
            index: isAppend(b) ? -1 : span.start + c + 1,
            row: from.row,
            lane: side,
          });
        }
        links.push({
          key: `${tail.id}->mix@${side}`,
          from: tail,
          to,
          wrap: false,
          // Appending to a lane is its [+] slot's job; without one (nothing
          // left to add) there is nothing to insert either.
          index: -1,
          row: from.row,
          lane: side,
        });
      }
      continue;
    }

    const wrap = from.row !== to.row;
    links.push({
      key: `${from.id}->${to.id}`,
      from,
      to,
      wrap,
      via: wrap ? gutter(from, to) : undefined,
      index: indexOf(to),
      row: from.row,
    });
  }

  return links;
}

/** The box every slot fits inside, with room for captions and note pills. */
export function contentBox(slots: readonly Slot[]): Box {
  if (slots.length === 0) return { x: 0, y: -CARD_H / 2, w: TERMINAL_W, h: CARD_H };
  let left = Infinity;
  let right = -Infinity;
  let top = Infinity;
  let bottom = -Infinity;
  for (const slot of slots) {
    if (slot.x < left) left = slot.x;
    if (slot.x + slot.w > right) right = slot.x + slot.w;
    if (slot.y - slot.h / 2 < top) top = slot.y - slot.h / 2;
    if (slot.y + slot.h / 2 > bottom) bottom = slot.y + slot.h / 2;
  }
  return {
    x: left,
    y: top - CONTENT_PAD_Y,
    w: right - left,
    h: bottom - top + CONTENT_PAD_Y * 2,
  };
}

/** The connector layer's box: the content, padded for cards dragged off it. */
export function connectorBox(content: Box): Box {
  return {
    x: content.x - CONNECTOR_PAD_X,
    y: content.y - CONNECTOR_PAD_Y,
    w: content.w + CONNECTOR_PAD_X * 2,
    h: content.h + CONNECTOR_PAD_Y * 2,
  };
}

/** The slot a node currently occupies, or undefined once it has left. */
export function slotOf(slots: readonly Slot[], id: NodeId): Slot | undefined {
  return slots.find((slot) => slot.id === id);
}

/** A hover-revealed [+] between two lane elements — "insert a block here". */
export interface InsertPoint {
  /** Flat chain index the new block lands at. */
  index: number;
  /** Row the insert grows (the earlier row on a wrap gap). */
  row: number;
  /** Centre of the gap, in lane units (`y` is a centre line, like a slot's). */
  x: number;
  y: number;
}

/**
 * One insertion point per connector gap the tail [+] node does not already
 * cover: after IN, between adjacent cards, in the gutter of a row wrap (halfway
 * along the wrap's return run) and — the reason this is derived from links
 * rather than from adjacent slots — inside a lane, including the bypass wire of
 * an empty one. Gaps into [+]/OUT are skipped: they mean "append", which the [+]
 * node owns, and carry `index: -1`.
 */
export function insertPoints(links: readonly Link[]): InsertPoint[] {
  const points: InsertPoint[] = [];
  for (const link of links) {
    if (link.index < 0) continue;
    points.push({
      index: link.index,
      row: link.row,
      x: (link.from.x + link.from.w + link.to.x) / 2,
      y: link.via ?? (link.from.y + link.to.y) / 2,
    });
  }
  return points;
}

/* ─────────────────────────── drop-target resolution ────────────────────── */

/** Centre line of the virtual "new row" strip below the last row. */
export function newRowY(geom: readonly RowGeom[]): number {
  const last = geom[geom.length - 1];
  return (last ? rowBottom(last) : CARD_H / 2) + ROW_GAP + CARD_H / 2;
}

/**
 * Row a pointer at lane-y `y` is over, against FROZEN row geometry.
 *
 * `geom.length` itself is a legal answer — that is the virtual new-row strip,
 * one row-height below the last row. Above row 0 the answer clamps to 0; below
 * the strip the caller cancels (see `isBelowRows`). Row bands meet in the middle
 * of the gutter between them, which for plain rows is the old
 * `round(y / ROW_STRIDE)` to the pixel.
 */
export function rowForY(geom: readonly RowGeom[], y: number): number {
  for (let row = 0; row < geom.length; row += 1) {
    const current = geom[row]!;
    const below = geom[row + 1];
    const edge = below
      ? (rowBottom(current) + rowTop(below)) / 2
      : (rowBottom(current) + newRowY(geom) - CARD_H / 2) / 2;
    if (y < edge) return row;
  }
  return geom.length;
}

/** True once the pointer has fallen past the new-row strip — a mouse cancel. */
export function isBelowRows(geom: readonly RowGeom[], y: number): boolean {
  return y > newRowY(geom) + ROW_STRIDE / 2;
}

/**
 * Insertion column for a card whose left edge is at `x`, in a row that holds
 * `count` cards. Ranges 0…count (count = "after the last card"), ties favouring
 * the lower column, exactly like the native view's nearest-slot rule.
 */
export function columnForX(x: number, count: number): number {
  const raw = (x - FIRST_CARD_X) / SLOT_STRIDE;
  return clamp(Math.round(raw), 0, count);
}

/** Where a drag would drop: a row (serial) or a lane track of a split container. */
export interface DropTarget {
  row: number;
  /** The lane track when the drop is inside a split container, null otherwise. */
  lane: LaneSide | null;
  /** Column within the row, or within the lane track. */
  column: number;
  /** Flat index the block lands at — always inside one legal segment, so a drop
   *  can never break the grammar. */
  index: number;
}

/**
 * Resolve a pointer position to a drop target against frozen geometry.
 *
 * `x` is the dragged card's LEFT edge (what the column grid measures). In a
 * row that holds the split region the column decides the segment: at or left
 * of SPLIT's column it is the serial run before the region, inside the lane
 * columns (the [+] slots included) it is a lane — the upper half of the row is
 * lane A, the lower half lane B — and from MIX's column on it is the serial run
 * after the region. Every answer is a flat index inside one legal segment.
 */
export function dropTargetAt(
  order: readonly BlockId[],
  geom: readonly RowGeom[],
  x: number,
  y: number,
): DropTarget {
  const row = rowForY(geom, y);
  const g = geom[row];
  if (!g) return { row, lane: null, column: 0, index: order.length };

  const spans = g.lanes ? laneSpans(findStructure(order)) : null;
  if (!g.lanes || !spans) {
    const column = columnForX(x, g.count);
    return { row, lane: null, column, index: g.start + column };
  }

  const structure = findStructure(order);
  const head = g.regionColumn;
  const mixColumn = head + LANE_FIRST_COLUMN + g.laneColumns;
  const end = g.start + g.count;
  const after = end - (structure.mixAt + 1);
  const raw = Math.round((x - FIRST_CARD_X) / SLOT_STRIDE);

  if (raw <= head) {
    const column = clamp(raw, 0, head);
    return { row, lane: null, column, index: g.start + column };
  }
  if (raw < mixColumn) {
    const lane: LaneSide = y < g.y ? "a" : "b";
    const span = spans[lane];
    const column = clamp(raw - head - LANE_FIRST_COLUMN, 0, span.count);
    return { row, lane, column, index: span.start + column };
  }
  const offset = clamp(raw - mixColumn - 1, 0, after);
  return {
    row,
    lane: null,
    column: mixColumn + 1 + offset,
    index: structure.mixAt + 1 + offset,
  };
}

export function clamp(value: number, min: number, max: number): number {
  return value < min ? min : value > max ? max : value;
}

/** Immutable insert used by the drag preview and the drop commit. */
export function insertItem<T>(items: readonly T[], index: number, item: T): T[] {
  const next = items.slice();
  next.splice(index, 0, item);
  return next;
}

/** Row lengths with one more block in `row`; past the end it appends a row. */
export function rowsWithInsert(rows: readonly number[], row: number): number[] {
  const next = rows.slice();
  if (row < next.length) next[row] = (next[row] ?? 0) + 1;
  else next.push(1);
  return next;
}

/** Row lengths with the block at `index` taken out; emptied rows collapse. */
export function rowsWithRemove(
  rows: readonly number[],
  index: number,
): number[] {
  const next = rows.slice();
  let start = 0;
  for (let row = 0; row < next.length; row += 1) {
    const length = next[row] ?? 0;
    if (index < start + length) {
      next[row] = length - 1;
      break;
    }
    start += length;
  }
  return next.filter((length) => length > 0);
}

/** Row lengths with `count` blocks taken out of `row`; an emptied row collapses. */
export function rowsWithoutRun(
  rows: readonly number[],
  row: number,
  count: number,
): number[] {
  const next = rows.slice();
  if (row >= 0 && row < next.length) next[row] = (next[row] ?? 0) - count;
  return next.filter((length) => length > 0);
}

/** Row lengths with `count` more blocks in `row`; past the end it appends a row. */
export function rowsWithRun(
  rows: readonly number[],
  row: number,
  count: number,
): number[] {
  const next = rows.slice();
  if (row < next.length) next[row] = (next[row] ?? 0) + count;
  else next.push(count);
  return next;
}

/** The row that holds flat index `index`; the last row past the end. */
export function rowContaining(rows: readonly number[], index: number): number {
  let start = 0;
  for (let row = 0; row < rows.length; row += 1) {
    start += rows[row] ?? 0;
    if (index < start) return row;
  }
  return Math.max(rows.length - 1, 0);
}

export function sameOrder(a: readonly string[], b: readonly string[]): boolean {
  return a.length === b.length && a.every((id, i) => id === b[i]);
}

export function sameRows(a: readonly number[], b: readonly number[]): boolean {
  return a.length === b.length && a.every((n, i) => n === b[i]);
}
