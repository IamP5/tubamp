/**
 * "Removed Reverb · Undo": a chain edit made from the board, followed by a toast
 * whose action puts the previous order and rows back in ONE `setChainOrder` —
 * the same single write every other chain edit makes.
 *
 * The undo only applies to the chain it was offered for: if anything changed
 * the order since (another add, a drag, a preset recall), restoring the old
 * order would silently throw that away, so the action does nothing then.
 */
import { useStore } from "../../store";
import { sameOrder, sameRows } from "./layout";

/** Long enough to read the toast and reach for its button. */
const UNDO_MS = 6000;

export function withUndo(message: string, change: () => void): void {
  const before = useStore.getState();
  const order = before.chainOrder;
  const rows = before.chainRows;
  change();

  const after = useStore.getState();
  const changedOrder = after.chainOrder;
  const changedRows = after.chainRows;
  if (sameOrder(changedOrder, order) && sameRows(changedRows, rows)) return;

  after.toast(message, "info", UNDO_MS, {
    label: "Undo",
    onSelect: () => {
      const now = useStore.getState();
      if (!sameOrder(now.chainOrder, changedOrder)) return;
      now.setChainOrder([...order], [...rows]);
    },
  });
}
