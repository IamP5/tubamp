/**
 * Manual drag-to-reorder for the chain lane (motion-design.md §2.2 / §3.2 —
 * deliberately NOT `Reorder.Group`: the lane lives on a pan/zoom canvas and has
 * to coordinate with connector redraw and the stage transform).
 *
 * Shape of a drag:
 *   pointerdown   → select the block (a plain click still selects, exactly like
 *                   the native view) and arm the gesture
 *   5px of travel → the gesture becomes a drag: the card lifts and stops being
 *                   laid out by the reflow pass; its `x` is written straight
 *                   from the pointer (screen delta / zoom = lane delta)
 *   crossing a slot centre → the *preview* order changes (local state only, one
 *                   `flushSync` so the rendered order and the motion values
 *                   never disagree for a frame); siblings spring to their slots
 *   pointerup     → commit ONCE via the store action, and only when the order
 *                   actually changed; the card settles onto its slot
 *   Escape / leaving the lane's expanded bounds → restore the pre-drag order and
 *                   publish nothing
 *
 * Reentrancy: the preview order is local, and the only write to the plugin is
 * the single `commit()` on drop, guarded by a value comparison against the
 * store. Applying an inbound `chainChanged` can therefore never bounce back out
 * as another `setChainOrder` (the native view needed an `applyingOrder` flag for
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
import { useStore } from "../../store";
import { spring } from "../../theme/motion";
import {
  CANCEL_PAD_X,
  CANCEL_PAD_Y,
  CARD_H,
  CARD_W,
  DRAG_LIFT,
  DRAG_THRESHOLD_PX,
  cardSlotX,
  clamp,
  computeSlots,
  moveItem,
  sameOrder,
  slotIndexForCenter,
  totalWidth,
} from "./layout";
import type { NodeMotionRegistry } from "./nodes";

interface DragSession {
  id: BlockId;
  pointerId: number;
  startClientX: number;
  startIndex: number;
  index: number;
  /** Live preview order — mutated in pointer handlers only. */
  order: BlockId[];
  moved: boolean;
}

export interface UseChainDragOptions {
  /** Store order — what renders when no drag is in flight. */
  chainOrder: BlockId[];
  showAdd: boolean;
  nodes: NodeMotionRegistry;
  /** Lane origin element (0×0, sits at lane-space (0, 0)). */
  laneRef: RefObject<HTMLDivElement | null>;
  getZoom(): number;
  /** True while the stage owns the pointer (space / middle-drag pan). */
  isPanMode(): boolean;
  onSelect(id: BlockId): void;
  /** Publishes the new order — called at most once per drag. */
  commit(order: BlockId[]): void;
  reduced: boolean;
}

export interface ChainDrag {
  /** Order to render: the live preview while dragging, the store order otherwise. */
  order: BlockId[];
  dragId: BlockId | null;
  beginDrag(event: ReactPointerEvent<HTMLElement>, id: BlockId): void;
}

export function useChainDrag({
  chainOrder,
  showAdd,
  nodes,
  laneRef,
  getZoom,
  isPanMode,
  onSelect,
  commit,
  reduced,
}: UseChainDragOptions): ChainDrag {
  const [preview, setPreview] = useState<BlockId[] | null>(null);
  const [dragId, setDragId] = useState<BlockId | null>(null);
  const session = useRef<DragSession | null>(null);

  const settle = useCallback(
    (id: BlockId, index: number) => {
      const node = nodes.get(id);
      nodes.freeze(id);
      const options = reduced ? { duration: 0 } : spring.settle;
      animate(node.x, cardSlotX(index), options);
      animate(node.y, 0, options);
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
        return;
      }

      // The store is the authority on what is currently published.
      const published = useStore.getState().chainOrder;

      if (mode === "cancel") {
        const index = published.indexOf(active.id);
        settle(active.id, index < 0 ? active.startIndex : index);
      } else {
        settle(active.id, active.index);
        if (!sameOrder(active.order, published)) commit(active.order);
      }

      flushSync(() => {
        setPreview(null);
        setDragId(null);
      });
    },
    [commit, settle],
  );

  /* One set of window listeners for the component's lifetime; they no-op unless
     a session is armed, which keeps gesture start/stop allocation-free. */
  useEffect(() => {
    const onMove = (event: PointerEvent): void => {
      const active = session.current;
      if (!active || event.pointerId !== active.pointerId) return;

      const dxScreen = event.clientX - active.startClientX;
      if (!active.moved) {
        if (Math.abs(dxScreen) < DRAG_THRESHOLD_PX) return;
        active.moved = true;
        setDragId(active.id);
        if (!reduced) {
          animate(nodes.get(active.id).y, -DRAG_LIFT, spring.pickup);
        }
      }

      const zoom = getZoom();
      const lane = laneRef.current?.getBoundingClientRect();
      const total = totalWidth(computeSlots(active.order, showAdd));

      if (lane) {
        // Leaving the lane's expanded bounds cancels — 24×48 tolerance, the same
        // gesture the native view used (there it was evaluated on release).
        const lx = (event.clientX - lane.left) / zoom;
        const ly = (event.clientY - lane.top) / zoom;
        const outside =
          lx < -CANCEL_PAD_X ||
          lx > total + CANCEL_PAD_X ||
          ly < -CARD_H / 2 - CANCEL_PAD_Y ||
          ly > CARD_H / 2 + CANCEL_PAD_Y;
        if (outside) {
          endSession("cancel");
          return;
        }
      }

      // The dragged card never leaves the lane horizontally (native parity).
      const x = clamp(
        cardSlotX(active.startIndex) + dxScreen / zoom,
        cardSlotX(0) - CANCEL_PAD_X,
        cardSlotX(active.order.length - 1) + CANCEL_PAD_X,
      );
      nodes.get(active.id).x.set(x);

      const target = slotIndexForCenter(x + CARD_W / 2, active.order.length);
      if (target !== active.index) {
        active.order = moveItem(active.order, active.index, target);
        active.index = target;
        // flushSync so the sibling reflow starts in the very frame the pointer
        // crossed the slot centre — a batched update would show a torn frame.
        const next = active.order;
        flushSync(() => setPreview(next));
      }
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
  }, [endSession, getZoom, laneRef, nodes, reduced, showAdd]);

  const beginDrag = useCallback(
    (event: ReactPointerEvent<HTMLElement>, id: BlockId) => {
      // Middle button and space-pan belong to the stage: let them bubble.
      if (event.button !== 0 || isPanMode() || session.current) return;
      const order = useStore.getState().chainOrder;
      const index = order.indexOf(id);
      if (index < 0) return;

      event.stopPropagation();
      onSelect(id);
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
        startIndex: index,
        index,
        order: order.slice(),
        moved: false,
      };
    },
    [isPanMode, onSelect],
  );

  return { order: preview ?? chainOrder, dragId, beginDrag };
}
