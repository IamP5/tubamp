/**
 * Board geometry — rows of blocks.
 *
 * The chain is laid out analytically rather than by DOM flow: every node (IN
 * terminal, block cards, [+] node, OUT terminal) is absolutely positioned by a
 * pair of `x`/`y` motion values, so reflow/drag/settle are pure transform
 * animations and connector geometry can be derived from the very same numbers —
 * no `getBoundingClientRect` in any per-frame path, and no Motion `layout`
 * projection inside the zoomed stage (which would double-apply the stage scale).
 *
 * Coordinate space: "lane units". x = 0 is the left edge of the IN terminal,
 * y = 0 is the centre line of the FIRST row; row i sits `ROW_STRIDE` below it.
 * `y` names a node's centre (every node carries `marginTop: -h / 2`), which is
 * what makes a straight connector between two same-row nodes a single `H`.
 *
 * The chain is one flat order; `rows` (from the store's `rowsFor`) is the
 * partition that says where it wraps. Rows are left-aligned on a SHARED column
 * grid: card k of every row sits at `cardSlotX(k)`, so the cards line up
 * vertically and one x→index rule serves every row. Row 0 additionally carries
 * IN at x = 0; the last row carries [+] and OUT after its last card.
 */
import type { BlockId } from "../../bridge/types";

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

/** Distance between two adjacent row centre lines. */
export const ROW_STRIDE = CARD_H + ROW_GAP;

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
}

/** Centre line of row `row`. */
export function rowY(row: number): number {
  return row * ROW_STRIDE;
}

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
 * IN — cards… — [+]? — OUT, wrapped into `rows`.
 *
 * `rows` must partition `order` exactly (the store's `rowsFor` guarantees it);
 * an empty chain still lays out one row, so IN/[+]/OUT have somewhere to sit.
 */
export function computeSlots(
  order: readonly BlockId[],
  rows: readonly number[],
  showAdd: boolean,
): Slot[] {
  const slots: Slot[] = [];
  const rowCount = Math.max(rows.length, 1);
  let index = 0;

  for (let row = 0; row < rowCount; row += 1) {
    const y = rowY(row);
    if (row === 0) slots.push({ id: "in", x: 0, y, w: TERMINAL_W, h: TERMINAL_H, row });

    const count = rows[row] ?? 0;
    for (let column = 0; column < count; column += 1) {
      const id = order[index];
      index += 1;
      if (id === undefined) break;
      slots.push({ id, x: cardSlotX(column), y, w: CARD_W, h: CARD_H, row });
    }

    if (row === rowCount - 1) {
      let x = cardSlotX(count);
      if (showAdd) {
        slots.push({ id: "add", x, y, w: ADD_W, h: ADD_H, row });
        x += ADD_W + GAP;
      }
      slots.push({ id: "out", x, y, w: TERMINAL_W, h: TERMINAL_H, row });
    }
  }
  return slots;
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
 * cover: after IN, between adjacent cards, and in the gutter of a row wrap
 * (halfway along the wrap's return run, which is `(x1+x2)/2` because the
 * lead-out and lead-in are symmetric). Gaps into [+]/OUT are skipped — they mean
 * "append", which the [+] node owns.
 */
export function insertPoints(slots: readonly Slot[]): InsertPoint[] {
  const points: InsertPoint[] = [];
  let index = 0;
  for (let i = 0; i < slots.length - 1; i += 1) {
    const from = slots[i]!;
    const to = slots[i + 1]!;
    // Blocks seen so far — i.e. the flat index of `to` while `to` is a block.
    if (from.id !== "in" && from.id !== "add" && from.id !== "out") index += 1;
    if (to.id === "add" || to.id === "out") continue;
    points.push({
      index,
      row: from.row,
      x: (from.x + from.w + to.x) / 2,
      y: (from.y + to.y) / 2,
    });
  }
  return points;
}

/**
 * Row a pointer at lane-y `y` is over, against a FROZEN row count.
 *
 * `rowCount` is the number of rows the drag started from; `rowCount` itself is a
 * legal answer — that is the virtual new-row strip, exactly one row-height below
 * the last row. Above row 0 the answer clamps to 0; below the strip the caller
 * cancels (see `isBelowRows`).
 */
export function rowForY(y: number, rowCount: number): number {
  return clamp(Math.round(y / ROW_STRIDE), 0, rowCount);
}

/** True once the pointer has fallen past the new-row strip — a mouse cancel. */
export function isBelowRows(y: number, rowCount: number): boolean {
  return y > rowCount * ROW_STRIDE + ROW_STRIDE / 2;
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

export function sameOrder(a: readonly string[], b: readonly string[]): boolean {
  return a.length === b.length && a.every((id, i) => id === b[i]);
}

export function sameRows(a: readonly number[], b: readonly number[]): boolean {
  return a.length === b.length && a.every((n, i) => n === b[i]);
}
