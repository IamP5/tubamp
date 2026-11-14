import type { Stage } from "./signal.ts";

/**
 * The chain chapter's whole state. The order is the state; the tiles, the
 * waveform and the caption are drawn from it. `waiting` is the board nobody has
 * touched yet: the one demonstration swap can only start from there, so it
 * plays at most once and never over something the visitor did.
 */
export type Phase =
  | { kind: "waiting" }
  | { kind: "demo" }
  | { kind: "idle" }
  | { kind: "dragging"; id: Stage };

/** What the last change did, for the caption. */
export type Note =
  | { kind: "rest" }
  | { kind: "tail" }
  | { kind: "first"; id: Stage }
  | { kind: "after"; id: Stage; other: Stage };

export type Board = { order: readonly Stage[]; phase: Phase; note: Note };

export type Action =
  | { type: "demo" }
  | { type: "demoEnd" }
  | { type: "grab"; id: Stage }
  | { type: "arrange"; order: readonly Stage[]; id: Stage }
  | { type: "drop" }
  | { type: "step"; id: Stage; by: -1 | 1 }
  | { type: "reset" };

export const DEMO_SWAP = ["drive", "reverb"] as const satisfies readonly Stage[];

export const initialBoard = (order: readonly Stage[]): Board => ({
  order,
  phase: { kind: "waiting" },
  note: { kind: "rest" },
});

const same = (a: readonly Stage[], b: readonly Stage[]) => a.length === b.length && a.every((s, i) => s === b[i]);

/** A reverb ahead of the drive sends its whole tail through the clipping. */
const tailDistorts = (order: readonly Stage[]) => {
  const reverb = order.indexOf("reverb");
  const drive = order.indexOf("drive");
  return reverb > -1 && drive > -1 && reverb < drive;
};

function noteFor(before: readonly Stage[], after: readonly Stage[], id: Stage, home: readonly Stage[]): Note {
  if (same(after, home)) return { kind: "rest" };
  if (tailDistorts(after) && !tailDistorts(before)) return { kind: "tail" };
  const at = after.indexOf(id);
  return at === 0 ? { kind: "first", id } : { kind: "after", id, other: after[at - 1] };
}

function swap(order: readonly Stage[], a: Stage, b: Stage): Stage[] {
  return order.map((s) => (s === a ? b : s === b ? a : s));
}

function stepped(order: readonly Stage[], id: Stage, by: -1 | 1): readonly Stage[] {
  const from = order.indexOf(id);
  const to = from + by;
  if (from < 0 || to < 0 || to >= order.length) return order;
  return swap(order, id, order[to]);
}

/** `home` is the default order: Reset returns to it and the caption knows it by name. */
export function reduce(board: Board, action: Action, home: readonly Stage[]): Board {
  const { order, phase } = board;
  const arrange = (next: readonly Stage[], id: Stage, to: Phase): Board =>
    same(next, order) ? { ...board, phase: to } : { order: next, phase: to, note: noteFor(order, next, id, home) };

  switch (action.type) {
    case "demo":
      return phase.kind === "waiting" ? arrange(swap(order, ...DEMO_SWAP), DEMO_SWAP[1], { kind: "demo" }) : board;
    case "demoEnd":
      return phase.kind === "demo" ? { ...board, phase: { kind: "idle" } } : board;
    case "grab":
      return { ...board, phase: { kind: "dragging", id: action.id } };
    case "arrange":
      return arrange(action.order, action.id, phase.kind === "dragging" ? phase : { kind: "idle" });
    case "drop":
      return phase.kind === "dragging" ? { ...board, phase: { kind: "idle" } } : board;
    case "step":
      return arrange(stepped(order, action.id, action.by), action.id, { kind: "idle" });
    case "reset":
      return { order: home, phase: { kind: "idle" }, note: { kind: "rest" } };
  }
}

/** The blocks to mark on the board: the one in the hand, or the two the demonstration swaps. */
export function lit(phase: Phase): readonly Stage[] {
  if (phase.kind === "dragging") return [phase.id];
  return phase.kind === "demo" ? DEMO_SWAP : [];
}

export const isHome = (board: Board, home: readonly Stage[]) => same(board.order, home);
