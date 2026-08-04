/**
 * Per-node motion values (position of every lane element) plus the reflow
 * animation that keeps them on their slots.
 *
 * These motion values are the single source of truth for where a lane element
 * *is* right now: cards render them through `style={{ x, y }}`, the drag writes
 * them directly, and the connector layer reads them on its own frame callback.
 * Nothing here ever re-renders React.
 */
import { useCallback, useMemo, useState } from "react";
import { animate, motionValue, type MotionValue } from "motion/react";
import { spring } from "../../theme/motion";
import type { NodeId, Slot } from "./layout";

export interface NodeMotion {
  x: MotionValue<number>;
  y: MotionValue<number>;
}

export interface NodeMotionRegistry {
  /** Motion values for `id`, created on first use. */
  get(id: NodeId): NodeMotion;
  /**
   * Exempt a node from the *next* reflow pass — used when a drop hands a card
   * to the settle spring, so the reflow pass does not immediately restate it.
   */
  freeze(id: NodeId): void;
  /**
   * Move every node onto its slot, in BOTH axes — a node's resting y is its
   * row's centre line, never 0. Nodes seen for the first time are placed
   * instantly (they are entering with their own entrance animation); everything
   * else springs. `sticky` ids are skipped every pass for as long as they are
   * passed in — that is the card currently under the pointer.
   */
  layout(
    slots: readonly Slot[],
    sticky: ReadonlySet<string>,
    reduced: boolean,
  ): void;
  /** Drop motion values for nodes that left the lane. */
  prune(slots: readonly Slot[]): void;
}

interface Registry {
  values: Map<NodeId, NodeMotion>;
  placed: Set<NodeId>;
  oneShot: Set<string>;
}

const INSTANT = { duration: 0 } as const;

export function useNodeMotion(): NodeMotionRegistry {
  const [registry] = useState<Registry>(() => ({
    values: new Map(),
    placed: new Set(),
    oneShot: new Set(),
  }));

  const get = useCallback(
    (id: NodeId): NodeMotion => {
      let entry = registry.values.get(id);
      if (!entry) {
        entry = { x: motionValue(0), y: motionValue(0) };
        registry.values.set(id, entry);
      }
      return entry;
    },
    [registry],
  );

  const freeze = useCallback(
    (id: NodeId) => {
      registry.oneShot.add(id);
    },
    [registry],
  );

  const layout = useCallback(
    (slots: readonly Slot[], sticky: ReadonlySet<string>, reduced: boolean) => {
      for (const slot of slots) {
        if (sticky.has(slot.id) || registry.oneShot.has(slot.id)) {
          registry.placed.add(slot.id);
          continue;
        }
        const node = get(slot.id);
        if (!registry.placed.has(slot.id)) {
          registry.placed.add(slot.id);
          node.x.set(slot.x);
          node.y.set(slot.y);
          continue;
        }
        if (node.x.get() !== slot.x) {
          animate(node.x, slot.x, reduced ? INSTANT : spring.reflow);
        }
        if (node.y.get() !== slot.y) {
          animate(node.y, slot.y, reduced ? INSTANT : spring.reflow);
        }
      }
      registry.oneShot.clear();
    },
    [get, registry],
  );

  const prune = useCallback(
    (slots: readonly Slot[]) => {
      const live = new Set<string>(slots.map((slot) => slot.id));
      for (const id of registry.placed) {
        if (!live.has(id)) {
          registry.placed.delete(id);
          registry.values.delete(id);
        }
      }
    },
    [registry],
  );

  // Every member is a `[registry]`-stable callback, so this identity never
  // changes — effects can depend on the registry without re-subscribing.
  return useMemo(
    () => ({ get, freeze, layout, prune }),
    [freeze, get, layout, prune],
  );
}
