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
 * structure, the whole region `split … mix` is regrouped into exactly ONE row
 * (`laneRows`) — one row-equivalent for the outer wrap math — and that row is
 * laid out as a container: SPLIT on the row's centre line at column 0, lane A's
 * cards on an upper track and lane B's on a lower track (both on the same column
 * grid, each track half the container's height), MIX on the centre line after
 * the longer lane. The container is LANE_ROW_H tall instead of CARD_H, which is
 * all the stage's fit/pan box needs to know — it measures slots, and the lane
 * cards' own extents are the container's extents.
 *
 * `lane2` has no slot: which lane a block is in is its POSITION relative to that
 * token in the flat order, so the token is furniture and never renders.
 */
import type { BlockId } from "../../bridge/types";
import { findStructure, laneSpans, type LaneSide } from "../../chain/structure";

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

/** First column a lane card can occupy — column 0 is the SPLIT tile. */
export const LANE_FIRST_COLUMN = 1;

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

export type NodeId = "in" | "out" | "add" | BlockId;

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
  /** This row is the split container: it holds exactly `split … mix`. */
  lanes: boolean;
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
 * The partition with the split region regrouped into exactly one row.
 *
 * `rows` is layout-only and tolerant, and neither the store's auto-wrap nor a
 * partition restored from C++ knows anything about structure — so a region can
 * arrive straddling two rows. The container is one row-equivalent, so the board
 * puts it back together here, keeping every other row break the user made.
 * Idempotent, and the identity function on a structureless order.
 *
 * Everything that needs to know where a block sits — geometry, drag, connectors,
 * the commit — reads the partition THROUGH this, exactly as it reads it through
 * the store's `rowsFor`.
 */
export function laneRows(
  order: readonly BlockId[],
  rows: readonly number[],
): number[] {
  const structure = findStructure(order);
  if (!structure.valid || structure.splitAt < 0 || rows.length === 0)
    return [...rows];

  const first = structure.splitAt;
  const last = structure.mixAt + 1;

  const cuts = new Set<number>();
  let at = 0;
  for (const length of rows) {
    at += length;
    if (at <= first || at >= last) cuts.add(at);
  }
  cuts.add(first);
  cuts.add(last);

  const next: number[] = [];
  let prev = 0;
  for (const cut of [...cuts].sort((a, b) => a - b)) {
    if (cut > prev && cut <= order.length) next.push(cut - prev);
    if (cut > prev) prev = cut;
  }
  if (prev < order.length) next.push(order.length - prev);
  return next;
}

/**
 * Per-row vertical geometry. A row is a split container only when it holds the
 * whole region — a caller that skipped `laneRows` gets plain rows rather than a
 * half-drawn container.
 */
export function rowGeometry(
  order: readonly BlockId[],
  rows: readonly number[],
): RowGeom[] {
  const structure = findStructure(order);
  const geom: RowGeom[] = [];
  const rowCount = Math.max(rows.length, 1);
  let start = 0;

  for (let row = 0; row < rowCount; row += 1) {
    const count = rows[row] ?? 0;
    const lanes =
      structure.valid &&
      structure.splitAt === start &&
      structure.mixAt === start + count - 1;
    const h = lanes ? LANE_ROW_H : CARD_H;
    const previous = geom[row - 1];
    const y =
      previous === undefined
        ? 0
        : previous.y + previous.h / 2 + ROW_GAP + h / 2;
    geom.push({ y, h, start, count, lanes });
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
  const geom = rowGeometry(order, rows);
  const structure = findStructure(order);
  const spans = laneSpans(structure);
  const slots: Slot[] = [];
  let region: LaneRegion | null = null;

  for (let row = 0; row < geom.length; row += 1) {
    const g = geom[row]!;
    const y = g.y;
    if (row === 0)
      slots.push({ id: "in", x: 0, y, w: TERMINAL_W, h: TERMINAL_H, row });

    let columns = 0;

    if (g.lanes && spans) {
      slots.push({ id: "split", x: cardSlotX(0), y, w: CARD_W, h: CARD_H, row });

      const trackA = y - LANE_OFFSET;
      const trackB = y + LANE_OFFSET;
      const lane = (span: typeof spans.a, at: number, side: LaneSide): void => {
        for (let i = 0; i < span.count; i += 1) {
          const id = order[span.start + i];
          if (id === undefined) break;
          slots.push({
            id,
            x: cardSlotX(LANE_FIRST_COLUMN + i),
            y: at,
            w: CARD_W,
            h: CARD_H,
            row,
            lane: side,
          });
        }
      };
      lane(spans.a, trackA, "a");
      lane(spans.b, trackB, "b");

      // The longer lane decides where MIX sits — but an empty lane still gets a
      // column's worth of track, or a passthrough fork would collapse into the
      // 44px gap between the two tiles with no room for its wire or its [+].
      const mixColumn =
        LANE_FIRST_COLUMN + Math.max(spans.a.count, spans.b.count, 1);
      slots.push({
        id: "mix",
        x: cardSlotX(mixColumn),
        y,
        w: CARD_W,
        h: CARD_H,
        row,
      });
      columns = mixColumn + 1;
      region = {
        head: "split",
        tail: "mix",
        tailW: CARD_W,
        y: rowTop(g),
        h: g.h,
        trackA,
        trackB,
      };
    } else {
      for (let i = 0; i < g.count; i += 1) {
        const id = order[g.start + i];
        if (id === undefined) break;
        // Never a card: a lane2 outside a container is a structure the board
        // could not draw, and it is about to be flattened by the sanitizer.
        if (id === "lane2") continue;
        slots.push({
          id,
          x: cardSlotX(columns),
          y,
          w: CARD_W,
          h: CARD_H,
          row,
        });
        columns += 1;
      }
    }

    if (row === geom.length - 1) {
      let x = cardSlotX(columns);
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
 * converges again. An empty lane still gets its own wire, bowed through the
 * track it stands for — that wire IS the empty lane, and its insert [+] is how
 * the first block gets into it.
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

  /** Where a block dropped in the gap before `slot` lands. */
  const indexOf = (slot: Slot): number =>
    slot.id === "add" || slot.id === "out"
      ? -1
      : order.indexOf(slot.id as BlockId);

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
        const cards = lanes[side];
        const span = spans[side];
        const head = cards[0];
        const tail = cards[cards.length - 1];

        if (!head || !tail) {
          links.push({
            key: `split->mix@${side}`,
            from,
            to,
            wrap: false,
            via: track[side],
            index: span.start,
            row: from.row,
          });
          continue;
        }

        links.push({
          key: `split->${head.id}`,
          from,
          to: head,
          wrap: false,
          index: span.start,
          row: from.row,
        });
        for (let c = 0; c < cards.length - 1; c += 1) {
          const a = cards[c]!;
          const b = cards[c + 1]!;
          links.push({
            key: `${a.id}->${b.id}`,
            from: a,
            to: b,
            wrap: false,
            index: span.start + c + 1,
            row: from.row,
          });
        }
        links.push({
          key: `${tail.id}->mix@${side}`,
          from: tail,
          to,
          wrap: false,
          index: span.end,
          row: from.row,
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
 * The split container's band is entirely lane: its upper half is lane A, its
 * lower half lane B (there is no serial section on that row — the row holds the
 * region and nothing else). A lane column is measured from column 1, since
 * column 0 is the SPLIT tile.
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
  if (g.lanes && spans) {
    const lane: LaneSide = y < g.y ? "a" : "b";
    const span = spans[lane];
    const raw = (x - cardSlotX(LANE_FIRST_COLUMN)) / SLOT_STRIDE;
    const column = clamp(Math.round(raw), 0, span.count);
    return { row, lane, column, index: span.start + column };
  }

  const column = columnForX(x, g.count);
  return { row, lane: null, column, index: g.start + column };
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
