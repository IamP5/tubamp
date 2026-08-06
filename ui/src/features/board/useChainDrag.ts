/**
 * Manual 2-D drag-to-reorder for the chain board (motion-design.md §2.2 / §3.2 —
 * deliberately NOT `Reorder.Group`: the board lives on a pan/zoom canvas and has
 * to coordinate with connector redraw and the stage transform).
 *
 * Shape of a drag:
 *   pointerdown   → arm the gesture. It does NOT select: the panel is an
 *                   overlay over the bottom of the board, and opening it under
 *                   the card you are about to pick up is exactly wrong.
 *   5px of travel → the gesture becomes a drag: the card lifts and stops being
 *                   laid out by the reflow pass; its `x`/`y` are written straight
 *                   from the pointer (screen delta / zoom = lane delta)
 *   moving        → the target is resolved against FROZEN geometry (see below);
 *                   when it changes, the *preview* order+rows change (local state
 *                   only, one `flushSync` so the rendered order and the motion
 *                   values never disagree for a frame) and siblings spring
 *   pointerup     → with travel: commit ONCE via the store action, and only when
 *                   the result actually differs from what is published; the card
 *                   settles onto its slot. Without travel: a plain click, which
 *                   selects the block.
 *   below the new-row strip, outside the stage, or Escape → restore the
 *                   published order and publish nothing
 *
 * FROZEN GEOMETRY. Row geometry is computed ONCE, at gesture start, from the
 * order MINUS the dragged card, and never again. The target is then (row from
 * the y-bands, insertion column from x) against that fixed layout. Resolving
 * against the *preview* instead would be circular — inserting into a row moves
 * that row's slot centres, which moves the nearest centre back out of it — and
 * with a flushSync per move each flip would be a rendered frame.
 *
 * SPLIT PATHS (docs/SPLIT.md §5). The same frozen-geometry rule extends to the
 * lanes: the split container's row band is entirely lane — upper half A, lower
 * half B — so an ordinary block (amp2 included) drops into either track exactly
 * the way it drops into a serial row, and `dropTargetAt` hands back a flat index
 * that is always inside one legal segment. The structural tiles themselves are
 * NOT draggable in v1 (the spec's stated simplification): SPLIT and MIX would
 * have to travel as a pair-preserving unit, and a half-moved pair is a chain
 * shape the sanitizer would flatten under the user's hands. They are removed
 * instead, which flattens the structure deliberately (see the board's `flatten`).
 * Every commit goes through `sanitizeStructure`, so even a preview built from a
 * chain that changed underneath cannot publish a malformed structure.
 *
 * Reentrancy: the preview is local, and the only write to the plugin is the
 * single `commit()` on drop, guarded by a value comparison against the store.
 * Applying an inbound `chainChanged` can therefore never bounce back out as
 * another `setChainOrder` (the native view needed an `applyingOrder` flag for
 * exactly this; unidirectional state replaces it).
 */
import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type PointerEvent as ReactPointerEvent,
  type RefObject,
} from "react";
import { flushSync } from "react-dom";
import { animate } from "motion/react";
import type { BlockId } from "../../bridge/types";
import { isStructureBlockId } from "../../bridge/types";
import { sanitizeStructure, type LaneSide } from "../../chain/structure";
import { rowsFor, useStore } from "../../store";
import { spring } from "../../theme/motion";
import {
  DRAG_LIFT,
  DRAG_THRESHOLD_PX,
  computeSlots,
  dropTargetAt,
  insertItem,
  isBelowRows,
  laneRows,
  rowGeometry,
  rowsWithInsert,
  rowsWithRemove,
  sameOrder,
  sameRows,
  slotOf,
  type RowGeom,
  type Slot,
} from "./layout";
import type { NodeMotionRegistry } from "./nodes";

interface DragSession {
  id: BlockId;
  pointerId: number;
  startClientX: number;
  startClientY: number;
  /** Where the card sat when the gesture began, in lane units. */
  startX: number;
  startY: number;
  /* ── frozen geometry: the board with the dragged card taken out ── */
  baseOrder: BlockId[];
  baseRows: number[];
  baseGeom: RowGeom[];
  /* ── the published chain the gesture started from; a mismatch at drop means
     something external (preset recall, host restore) replaced the chain mid-drag
     and the preview was built from a world that no longer exists ── */
  startOrder: BlockId[];
  startRows: number[];
  /* ── live target; -1 until the first move resolves one ── */
  row: number;
  column: number;
  /** Lane track of the target, when it is inside a split container. */
  lane: LaneSide | null;
  order: BlockId[];
  rows: number[];
  moved: boolean;
}

export interface UseChainDragOptions {
  /** Store order — what renders when no drag is in flight. */
  chainOrder: BlockId[];
  /** Rows AS DISPLAYED (auto-wrap already resolved), not the raw store value. */
  chainRows: number[];
  showAdd: boolean;
  nodes: NodeMotionRegistry;
  /** Lane origin element (0×0, sits at lane-space (0, 0)). */
  laneRef: RefObject<HTMLDivElement | null>;
  /** The stage viewport — leaving it cancels the drag (mouse-only cancel). */
  stageRef: RefObject<HTMLDivElement | null>;
  getZoom(): number;
  /** True while the stage owns the pointer (space / middle-drag pan). */
  isPanMode(): boolean;
  /** A press that never became a drag — a click, which selects. */
  onSelect(id: BlockId): void;
  /** Publishes the new order + rows — called at most once per drag. */
  commit(order: BlockId[], rows: number[]): void;
  reduced: boolean;
}

export interface ChainDrag {
  /** Order to render: the live preview while dragging, the store order otherwise. */
  order: BlockId[];
  /** Rows to render, in the same sense as `order`. */
  rows: number[];
  dragId: BlockId | null;
  beginDrag(event: ReactPointerEvent<HTMLElement>, id: BlockId): void;
}

interface Preview {
  order: BlockId[];
  rows: number[];
}

export function useChainDrag({
  chainOrder,
  chainRows,
  showAdd,
  nodes,
  laneRef,
  stageRef,
  getZoom,
  isPanMode,
  onSelect,
  commit,
  reduced,
}: UseChainDragOptions): ChainDrag {
  const [preview, setPreview] = useState<Preview | null>(null);
  const [dragId, setDragId] = useState<BlockId | null>(null);
  const session = useRef<DragSession | null>(null);

  const settle = useCallback(
    (id: BlockId, slot: Slot | undefined) => {
      if (!slot) return;
      const node = nodes.get(id);
      nodes.freeze(id);
      const options = reduced ? { duration: 0 } : spring.settle;
      animate(node.x, slot.x, options);
      animate(node.y, slot.y, options);
    },
    [nodes, reduced],
  );

  const endSession = useCallback(
    (mode: "commit" | "cancel") => {
      const active = session.current;
      if (!active) return;
      session.current = null;

      if (!active.moved) {
        setDragId(null);
        // A press with no travel is a click. Selecting here rather than on
        // pointerdown keeps the panel from opening over the card being picked up.
        if (mode === "commit") onSelect(active.id);
        return;
      }

      // The store is the authority on what is currently published. Through
      // `laneRows`, so the comparison below is against the partition the board
      // is actually showing — the published one may put the split region across
      // two rows, and the drag never worked in those terms.
      const state = useStore.getState();
      const publishedRows = laneRows(
        state.chainOrder,
        rowsFor(state.chainOrder, state.chainRows),
      );

      // The chain changed underneath the gesture (preset recall, A/B, host
      // restore): committing the preview would overwrite that change with a
      // layout built from the old chain. Keep the external result instead.
      const baselineIntact =
        sameOrder(active.startOrder, state.chainOrder) &&
        sameRows(active.startRows, publishedRows);

      if (mode === "cancel" || !baselineIntact) {
        const slots = computeSlots(state.chainOrder, publishedRows, showAdd);
        settle(active.id, slotOf(slots, active.id));
      } else {
        // Belt: the preview is built by inserting into one legal segment, so
        // this is the identity — but no order leaves this hook unsanitized.
        // A sanitize that actually removed something invalidates the partition,
        // and rows are layout-only: drop them to auto rather than publish a
        // partition that no longer counts the chain.
        const order = sanitizeStructure(active.order);
        const rows = order.length === active.order.length ? active.rows : [];
        const slots = computeSlots(order, rows, showAdd);
        settle(active.id, slotOf(slots, active.id));
        const changed =
          !sameOrder(order, state.chainOrder) || !sameRows(rows, publishedRows);
        if (changed) {
          // A chain that was on auto-wrap and still matches it stays on auto:
          // publishing the wrap as an explicit partition would freeze a shape
          // the user never arranged. A split container never matches the plain
          // wrap, so a structured board always publishes its rows — which is
          // right: the container has to own a row of its own.
          const auto =
            state.chainRows.length === 0 &&
            sameRows(rows, rowsFor(order, []));
          commit(order, auto ? [] : rows);
        }
      }

      flushSync(() => {
        setPreview(null);
        setDragId(null);
      });
    },
    [commit, onSelect, settle, showAdd],
  );

  /* One set of window listeners for the component's lifetime; they no-op unless
     a session is armed, which keeps gesture start/stop allocation-free. */
  useEffect(() => {
    const onMove = (event: PointerEvent): void => {
      const active = session.current;
      if (!active || event.pointerId !== active.pointerId) return;

      const dxScreen = event.clientX - active.startClientX;
      const dyScreen = event.clientY - active.startClientY;
      if (!active.moved) {
        if (
          Math.abs(dxScreen) < DRAG_THRESHOLD_PX &&
          Math.abs(dyScreen) < DRAG_THRESHOLD_PX
        )
          return;
        active.moved = true;
        setDragId(active.id);
      }

      const zoom = getZoom();

      // Mouse cancel #1: the pointer left the board. Logic's WebView has no
      // reliable keyboard, so Escape cannot be the only way out of a drag.
      const stage = stageRef.current?.getBoundingClientRect();
      if (
        stage &&
        (event.clientX < stage.left ||
          event.clientX > stage.right ||
          event.clientY < stage.top ||
          event.clientY > stage.bottom)
      ) {
        endSession("cancel");
        return;
      }

      // The card follows the pointer in both axes, lifted by DRAG_LIFT.
      const node = nodes.get(active.id);
      const x = active.startX + dxScreen / zoom;
      const y = active.startY + dyScreen / zoom - (reduced ? 0 : DRAG_LIFT);
      node.x.set(x);
      node.y.set(y);

      const lane = laneRef.current?.getBoundingClientRect();
      if (!lane) return;
      const laneY = (event.clientY - lane.top) / zoom;

      // Mouse cancel #2: below the new-row strip — a flick towards the footer
      // is "put it back", not "make a row down there".
      if (isBelowRows(active.baseGeom, laneY)) {
        endSession("cancel");
        return;
      }

      // Serial row or lane track — one rule, resolved against frozen geometry.
      const target = dropTargetAt(active.baseOrder, active.baseGeom, x, laneY);
      if (
        target.row === active.row &&
        target.column === active.column &&
        target.lane === active.lane
      )
        return;

      active.row = target.row;
      active.column = target.column;
      active.lane = target.lane;
      active.order = insertItem(active.baseOrder, target.index, active.id);
      // `laneRows` again: inserting inside the container keeps the region
      // contiguous, so this is the identity — but the partition the preview
      // renders with is never allowed to be one the board cannot draw.
      active.rows = laneRows(
        active.order,
        rowsWithInsert(active.baseRows, target.row),
      );
      // flushSync so the sibling reflow starts in the very frame the pointer
      // crossed the boundary — a batched update would show a torn frame.
      const next: Preview = { order: active.order, rows: active.rows };
      flushSync(() => setPreview(next));
    };

    const onUp = (event: PointerEvent): void => {
      const active = session.current;
      if (!active || event.pointerId !== active.pointerId) return;
      endSession("commit");
    };

    const onCancel = (): void => endSession("cancel");

    const onKeyDown = (event: KeyboardEvent): void => {
      if (event.key === "Escape" && session.current) {
        event.preventDefault();
        endSession("cancel");
      }
    };

    window.addEventListener("pointermove", onMove);
    window.addEventListener("pointerup", onUp);
    window.addEventListener("pointercancel", onCancel);
    window.addEventListener("keydown", onKeyDown);
    return () => {
      window.removeEventListener("pointermove", onMove);
      window.removeEventListener("pointerup", onUp);
      window.removeEventListener("pointercancel", onCancel);
      window.removeEventListener("keydown", onKeyDown);
    };
  }, [endSession, getZoom, laneRef, nodes, reduced, stageRef]);

  const beginDrag = useCallback(
    (event: ReactPointerEvent<HTMLElement>, id: BlockId) => {
      // Middle button and space-pan belong to the stage: let them bubble.
      if (event.button !== 0 || isPanMode() || session.current) return;
      // SPLIT / MIX / lane2 are structure, not blocks — see the header note.
      if (isStructureBlockId(id)) return;
      const order = useStore.getState().chainOrder;
      const index = order.indexOf(id);
      if (index < 0) return;

      const slots = computeSlots(order, chainRows, showAdd);
      const from = slotOf(slots, id);
      if (!from) return;

      // Frozen geometry: the board as it will look with this card taken out.
      // Resolved once here and never again — see the header note.
      const baseOrder = order.filter((block) => block !== id);
      const baseRows = laneRows(baseOrder, rowsWithRemove(chainRows, index));
      const baseGeom = rowGeometry(baseOrder, baseRows);

      event.stopPropagation();
      try {
        // Capture is a nicety (window listeners drive the gesture either way);
        // synthetic/unknown pointer ids throw here and must not break the drag.
        event.currentTarget.setPointerCapture(event.pointerId);
      } catch {
        /* ignore */
      }
      session.current = {
        id,
        pointerId: event.pointerId,
        startClientX: event.clientX,
        startClientY: event.clientY,
        startX: from.x,
        startY: from.y,
        baseOrder,
        baseRows,
        baseGeom,
        startOrder: order.slice(),
        startRows: chainRows.slice(),
        // No target yet: the first move resolves one and publishes the preview
        // unconditionally, so what is rendered and the frozen geometry agree
        // from the first frame — including the row this card is vacating, which
        // collapses right then rather than one boundary crossing later.
        row: -1,
        column: -1,
        lane: null,
        order: order.slice(),
        rows: chainRows.slice(),
        moved: false,
      };
    },
    [chainRows, isPanMode, showAdd],
  );

  return {
    order: preview?.order ?? chainOrder,
    rows: preview?.rows ?? chainRows,
    dragId,
    beginDrag,
  };
}
