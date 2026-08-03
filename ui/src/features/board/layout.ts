/**
 * Lane geometry.
 *
 * The chain is laid out analytically rather than by DOM flow: every node (IN
 * terminal, block cards, [+] node, OUT terminal) is absolutely positioned by a
 * single `x` motion value, so reflow/drag/settle are pure transform animations
 * and connector geometry can be derived from the very same numbers — no
 * `getBoundingClientRect` in any per-frame path, and no Motion `layout`
 * projection inside the zoomed stage (which would double-apply the stage scale).
 *
 * Coordinate space: "lane units". x = 0 is the left edge of the IN terminal,
 * y = 0 is the shared vertical centre line of every node. The lane is centred on
 * the stage by translating its container by `-totalWidth / 2`.
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

/** x of the first card slot: IN terminal + one gap. */
export const FIRST_CARD_X = TERMINAL_W + GAP;

/**
 * Rows at the bottom of the board covered by the param dock overlay.
 *
 * The board paints full-bleed (the dot grid runs behind and around the floating
 * dock card), but the lane is centred — and zoom-to-fit framed — inside the band
 * ABOVE the dock, so a chain never settles underneath it.
 *
 * MUST equal `--dock-inset` in theme/tokens.css (`--dock-h` + `--dock-gap`).
 */
export const DOCK_INSET = 244;

/** Vertical lift applied to a card while it is being dragged. */
export const DRAG_LIFT = 6;

/** Pointer travel (screen px) before a press turns into a reorder drag. */
export const DRAG_THRESHOLD_PX = 5;

/**
 * Cancel tolerance around the lane, in lane units — parity with the native view
 * (`getLocalBounds().expanded(24, 48)`).
 */
export const CANCEL_PAD_X = 24;
export const CANCEL_PAD_Y = 48;

/**
 * Box the connector SVG occupies inside the lane container, in lane units. It is
 * generously oversized (a full 9-block chain is ~1670 wide) and offset so that
 * SVG user-space (0, 0) — after the inner `translate` — is the lane origin.
 */
export const CONNECTOR_SVG = { left: -400, top: -160, w: 2400, h: 320 } as const;

/**
 * `icons::makeFlowArrow()` from the deleted BlockIcons.h, scaled to the same 7px
 * box the native painter used and re-centred on the origin so it can be
 * translated along a connector.
 */
export const FLOW_ARROW_PATH_D = "M-1.82 -2.38L1.82 0L-1.82 2.38";

export type NodeId = "in" | "out" | "add" | BlockId;

export interface Slot {
  id: NodeId;
  /** Left edge in lane units. */
  x: number;
  w: number;
  h: number;
}

/** IN — cards… — [+]? — OUT, left to right. */
export function computeSlots(
  order: readonly BlockId[],
  showAdd: boolean,
): Slot[] {
  const slots: Slot[] = [];
  let x = 0;
  const push = (id: NodeId, w: number, h: number): void => {
    slots.push({ id, x, w, h });
    x += w + GAP;
  };

  push("in", TERMINAL_W, TERMINAL_H);
  for (const id of order) push(id, CARD_W, CARD_H);
  if (showAdd) push("add", ADD_W, ADD_H);
  push("out", TERMINAL_W, TERMINAL_H);
  return slots;
}

export function totalWidth(slots: readonly Slot[]): number {
  const last = slots[slots.length - 1];
  return last ? last.x + last.w : 0;
}

/** Resting x of the card at `index` (independent of what else is in the lane). */
export function cardSlotX(index: number): number {
  return FIRST_CARD_X + index * SLOT_STRIDE;
}

/**
 * Slot a dragged card belongs in, resolved by its centre-x — "nearest slot
 * wins", ties favouring the lower index, exactly like the native view.
 */
export function slotIndexForCenter(centerX: number, count: number): number {
  if (count <= 1) return 0;
  const raw = (centerX - CARD_W / 2 - FIRST_CARD_X) / SLOT_STRIDE;
  return clamp(Math.round(raw), 0, count - 1);
}

export function clamp(value: number, min: number, max: number): number {
  return value < min ? min : value > max ? max : value;
}

/** Immutable splice-move used by both the drag and the drop commit. */
export function moveItem<T>(items: readonly T[], from: number, to: number): T[] {
  const next = items.slice();
  const [item] = next.splice(from, 1);
  if (item !== undefined) next.splice(to, 0, item);
  return next;
}

export function sameOrder(a: readonly string[], b: readonly string[]): boolean {
  return a.length === b.length && a.every((id, i) => id === b[i]);
}
