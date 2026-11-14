/**
 * The split-region grammar (docs/SPLIT.md §1), in TypeScript.
 *
 * EXACT mirror of `tubamp::chain::findStructure` / `tubamp::chain::sanitizeStructure`
 * (src/dsp/ChainOrder.h). A chain holds at most one split region, shaped
 *
 *     ..., split, <lane A ids...>, lane2, <lane B ids...>, mix, ...
 *
 * and the flat order is the ONLY thing that encodes it — there is no side channel,
 * here or in C++.
 *
 * C++ sanitizes every order that reaches the packed word, so this mirror is not what
 * keeps the audio thread safe; it is what keeps the UI from ever ASKING for something
 * the backstop would have to flatten, because a flatten the user did not request reads
 * as the board losing their lanes. Keep the two implementations identical: same
 * priority order, same tolerant degrade, never throws.
 */
import { isStructureBlockId, type BlockId } from "../bridge/types";

/**
 * The triple "Add split" inserts: an empty fork, and the smallest legal region.
 * Membership of the structural set lives in `bridge/types` (it is part of the
 * data contract); what is written here is the ORDER, which is the grammar.
 */
export const SPLIT_TRIPLE: readonly BlockId[] = ["split", "lane2", "mix"];

/** Mirror of `chain::maxChainLength`: a chosen cap, not an id-space consequence. */
export const MAX_CHAIN_LENGTH = 24;

/** Positions of the three structural ids in an order, or -1 where absent.
 *  `valid` is the C++ `Structure::valid()` — either no structure at all, or a
 *  well-formed one. */
export interface Structure {
  splitAt: number;
  lane2At: number;
  mixAt: number;
  valid: boolean;
}

/**
 * First occurrence of split / lane2 / mix, whatever shape the order is in. Callers
 * judge the result by `valid` (or re-derive it through `sanitizeStructure` first).
 */
export function findStructure(order: readonly BlockId[]): Structure {
  let splitAt = -1;
  let lane2At = -1;
  let mixAt = -1;

  for (let i = 0; i < order.length; i += 1) {
    const id = order[i];
    if (id === "split" && splitAt < 0) splitAt = i;
    else if (id === "lane2" && lane2At < 0) lane2At = i;
    else if (id === "mix" && mixAt < 0) mixAt = i;
  }

  const empty = splitAt < 0 && lane2At < 0 && mixAt < 0;
  return {
    splitAt,
    lane2At,
    mixAt,
    valid:
      empty ||
      (splitAt >= 0 &&
        lane2At >= 0 &&
        mixAt >= 0 &&
        splitAt < lane2At &&
        lane2At < mixAt),
  };
}

/**
 * Enforces the grammar, tolerantly, in this priority order:
 *   1. A repeated structural id keeps only its first occurrence.
 *   2. split < lane2 < mix all present → the structure is kept as-is.
 *   3. Otherwise every structural id is stripped and the order flattens to serial.
 *      `amp2` is NOT structural — it is an ordinary block and survives the flatten
 *      exactly where it sat.
 * Never throws: a garbled structure degrades to no structure.
 */
export function sanitizeStructure(order: readonly BlockId[]): BlockId[] {
  const deduped: BlockId[] = [];
  let sawSplit = false;
  let sawLane2 = false;
  let sawMix = false;

  for (const id of order) {
    if (id === "split") {
      if (sawSplit) continue;
      sawSplit = true;
    } else if (id === "lane2") {
      if (sawLane2) continue;
      sawLane2 = true;
    } else if (id === "mix") {
      if (sawMix) continue;
      sawMix = true;
    }
    deduped.push(id);
  }

  if (findStructure(deduped).valid) return deduped;
  return deduped.filter((id) => !isStructureBlockId(id));
}

/* ─────────────────────────── lane membership ───────────────────────────── */

export type LaneSide = "a" | "b";

/** Where one lane's blocks live in the flat order: `[start, start + count)`. An
 *  empty lane is legal (`split, lane2, mix` is a passthrough fork). */
export interface LaneSpan {
  start: number;
  count: number;
  /** Flat index a block appended to this lane lands at (= `start + count`, which
   *  is `lane2At` for lane A and `mixAt` for lane B). */
  end: number;
}

export interface LaneSpans {
  a: LaneSpan;
  b: LaneSpan;
}

/** The two lanes' spans, or null when the order carries no valid structure. */
export function laneSpans(structure: Structure): LaneSpans | null {
  if (!structure.valid || structure.splitAt < 0) return null;
  const { splitAt, lane2At, mixAt } = structure;
  return {
    a: { start: splitAt + 1, count: lane2At - splitAt - 1, end: lane2At },
    b: { start: lane2At + 1, count: mixAt - lane2At - 1, end: mixAt },
  };
}

/** True while a split region can still be created: none exists yet, and the cap
 *  has room for the three tokens it costs. */
export function canAddSplit(order: readonly BlockId[]): boolean {
  const { splitAt, lane2At, mixAt } = findStructure(order);
  if (splitAt >= 0 || lane2At >= 0 || mixAt >= 0) return false;
  return order.length + SPLIT_TRIPLE.length <= MAX_CHAIN_LENGTH;
}

/**
 * The order with the structure taken out — lane A's blocks then lane B's, inline
 * at the split's position, which is exactly what dropping the structural tokens
 * out of the flat order already does. Deleting SPLIT or MIX does this.
 */
export function flattenStructure(order: readonly BlockId[]): BlockId[] {
  return order.filter((id) => !isStructureBlockId(id));
}

/** The order with an empty fork inserted at `index` (clamped into range). */
export function insertSplit(
  order: readonly BlockId[],
  index: number,
): BlockId[] {
  const at = Math.min(Math.max(index, 0), order.length);
  return [...order.slice(0, at), ...SPLIT_TRIPLE, ...order.slice(at)];
}

/* ───────────────────────── whole-region edits ──────────────────────────── *
 * Every helper below takes a published order and hands back the COMPLETE next
 * order, already valid by `sanitizeStructure` — the caller publishes it in one
 * `setChainOrder`, so a gesture stays one write. None of them throws: an order
 * without a valid structure comes back flattened (removals) or unchanged
 * (swap), exactly what the sanitizer would have made of it.
 */

/** How a split region is taken out of the chain. */
export type SplitRemoval = "keepA" | "keepB" | "series";

/**
 * The order with the region removed:
 *   keepA  — lane A's blocks stay inline where the split stood, lane B's leave
 *            the chain (their parameters are untouched, only the tokens go);
 *   keepB  — the mirror image;
 *   series — lane A then lane B, inline (= `flattenStructure`). Right for an fx
 *            lane, wrong for a dual-amp rig, which is why it is not the only one.
 */
export function removeSplit(
  order: readonly BlockId[],
  how: SplitRemoval,
): BlockId[] {
  const spans = laneSpans(findStructure(order));
  if (!spans || how === "series") return flattenStructure(order);
  const drop = how === "keepA" ? spans.b : spans.a;
  return flattenStructure(
    order.filter((_, i) => i < drop.start || i >= drop.end),
  );
}

/** The blocks `removeSplit(order, how)` takes out of the chain — what a menu
 *  should name before the user commits to it. */
export function blocksDroppedBy(
  order: readonly BlockId[],
  how: SplitRemoval,
): BlockId[] {
  const spans = laneSpans(findStructure(order));
  if (!spans || how === "series") return [];
  const drop = how === "keepA" ? spans.b : spans.a;
  return order.slice(drop.start, drop.end);
}

/** The order with lane A's and lane B's contents exchanged (the `lane2` boundary
 *  moves with them). Unchanged when there is no valid structure. */
export function swapLanes(order: readonly BlockId[]): BlockId[] {
  const structure = findStructure(order);
  const spans = laneSpans(structure);
  if (!spans) return [...order];
  return [
    ...order.slice(0, structure.splitAt + 1),
    ...order.slice(spans.b.start, spans.b.end),
    "lane2",
    ...order.slice(spans.a.start, spans.a.end),
    ...order.slice(structure.mixAt),
  ];
}

/**
 * The one-click dual-amp rig: `amp` in lane A, `amp2` in lane B.
 *
 *  - `amp` already in the (structureless) chain → it is wrapped where it is:
 *    `[..., split, amp, lane2, amp2, mix, ...]` (four new tokens);
 *  - no `amp` → the whole rig is inserted at `index` (five new tokens).
 *
 * Null whenever the rig cannot be built: a region already exists (one per
 * chain), `amp2` is already placed somewhere, or the length cap has no room.
 * `insertAt` reports where the region's first token landed, for row bookkeeping.
 */
export function dualAmpOrder(
  order: readonly BlockId[],
  index: number,
): { order: BlockId[]; insertAt: number; added: number } | null {
  const { splitAt, lane2At, mixAt } = findStructure(order);
  if (splitAt >= 0 || lane2At >= 0 || mixAt >= 0) return null;
  if (order.includes("amp2")) return null;
  const rig: BlockId[] = ["split", "amp", "lane2", "amp2", "mix"];
  const ampAt = order.indexOf("amp");
  if (ampAt >= 0) {
    if (order.length + rig.length - 1 > MAX_CHAIN_LENGTH) return null;
    return {
      order: [...order.slice(0, ampAt), ...rig, ...order.slice(ampAt + 1)],
      insertAt: ampAt,
      added: rig.length - 1,
    };
  }
  if (order.length + rig.length > MAX_CHAIN_LENGTH) return null;
  const at = Math.min(Math.max(index, 0), order.length);
  return {
    order: [...order.slice(0, at), ...rig, ...order.slice(at)],
    insertAt: at,
    added: rig.length,
  };
}
