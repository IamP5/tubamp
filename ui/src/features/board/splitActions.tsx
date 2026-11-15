/**
 * Whole-region split actions, shared by the board (card menus, pickers) and
 * the Splitter / Mixer panels.
 *
 * Every chain edit here is ONE `setChainOrder` with an order that is already
 * valid by `sanitizeStructure` (the pure helpers in chain/structure.ts produce
 * them), plus row bookkeeping: an auto chain stays auto, an explicit partition
 * grows or shrinks the region's own row — the region never straddles a wrap,
 * so every token it gains or loses is in that one row.
 *
 * Parameter writes (pans, split mode) go through the bridge relays exactly the
 * way a knob writes them — one begin/set/end gesture each, so host automation
 * sees a clean touch per value.
 */
import { bridge, type BlockId, type SliderParamId } from "../../bridge";
import {
  blocksDroppedBy,
  dualAmpOrder,
  findStructure,
  removeSplit,
  swapLanes,
  type SplitRemoval,
} from "../../chain/structure";
import { copyAmpSettings, useStore } from "../../store";
import { BLOCK_INFO } from "../../theme/blocks";
import type { MenuEntry } from "../../components";
import {
  laneRows,
  rowContaining,
  rowsWithRun,
  rowsWithoutRun,
} from "./layout";
import { SPLIT_COPY, type LaneSide } from "./lanes";
import { withUndo } from "./undo";
import s from "./board.module.css";

/** The published partition through `laneRows`, or null while the chain is on
 *  auto rows (which stay auto through every edit below). */
function explicitRows(order: readonly BlockId[], chainRows: readonly number[]) {
  return chainRows.length > 0 ? laneRows(order, chainRows) : null;
}

/** Write a slider in parameter units, as one gesture. */
function writeSlider(id: SliderParamId, scaled: number): void {
  const state = bridge.sliderState(id);
  const { start, end, skew } = state.properties;
  const span = end - start;
  const proportion = span > 0 ? (scaled - start) / span : 0;
  state.beginGesture();
  state.setNormalisedValue(Math.pow(Math.min(1, Math.max(0, proportion)), skew));
  state.endGesture();
}

/** Both lane pans at once (-1 = hard left … +1 = hard right). */
export function setLanePans(a: number, b: number): void {
  writeSlider("mix_apan", a);
  writeSlider("mix_bpan", b);
}

/** Take the split region out of the chain — see `removeSplit`. */
export function removeSplitRegion(how: SplitRemoval): void {
  const state = useStore.getState();
  const order = state.chainOrder;
  const structure = findStructure(order);
  if (structure.splitAt < 0 && structure.lane2At < 0 && structure.mixAt < 0)
    return;

  const next = removeSplit(order, how);
  const shown = explicitRows(order, state.chainRows);
  const rows =
    shown === null || !structure.valid
      ? []
      : rowsWithoutRun(
          shown,
          rowContaining(shown, structure.splitAt),
          order.length - next.length,
        );
  state.setChainOrder(next, rows);
}

/** Exchange the contents of lane A and lane B (one write; rows unchanged —
 *  the region keeps its token count and its row). */
export function swapSplitLanes(): void {
  const state = useStore.getState();
  const next = swapLanes(state.chainOrder);
  state.setChainOrder(next, state.chainRows);
}

/** True when "Dual amp (stereo)" can be built from `order`. */
export function canBuildDualAmp(order: readonly BlockId[]): boolean {
  return dualAmpOrder(order, order.length) !== null;
}

/**
 * The one-click dual-amp rig: amp in lane A, amp2 in lane B (see
 * `dualAmpOrder`), then split mode Copy, lane A hard left, lane B hard right,
 * and the second amp starting as a copy of the first: its capture when it has
 * none, and every control, so both lanes sound right and loudness-matched
 * immediately. amp2 is selected so its panel opens on the second-capture
 * picker. `at` is the picker's position; it only matters when
 * the chain has no amp to wrap.
 */
export function buildDualAmp(at?: { index: number; row: number }): boolean {
  const state = useStore.getState();
  const order = state.chainOrder;
  const built = dualAmpOrder(order, at ? at.index : order.length);
  if (!built) return false;

  const shown = explicitRows(order, state.chainRows);
  let rows: number[] = [];
  if (shown !== null) {
    const wrapsAmp = order.includes("amp");
    const row =
      !wrapsAmp && at
        ? at.row
        : rowContaining(shown, Math.min(built.insertAt, order.length - 1));
    rows = laneRows(built.order, rowsWithRun(shown, row, built.added));
  }

  state.setChainOrder(built.order, rows);

  // Split and mixer parameters are process-wide and outlive a region that was
  // taken apart: a new dual-amp rig built on top of a muted lane B, an inverted
  // one or a pulled-down level would come up silent on one side, or cancelling.
  bridge.comboState("split_mode").setChoiceIndex(SPLIT_COPY);
  bridge.toggleState("split_on").setValue(true);
  bridge.toggleState("mix_on").setValue(true);
  bridge.toggleState("mix_bphase").setValue(false);
  writeSlider("mix_alevel", 0);
  writeSlider("mix_blevel", 0);
  writeSlider("mix_level", 0);
  setLanePans(-1, 1);
  const { model, modelB, loadModel } = state;
  if (model && !modelB) void loadModel(model.path, "amp2");
  copyAmpSettings("amp", "amp2");
  useStore.getState().selectBlock("amp2");
  return true;
}

/* ───────────────────────────── menu entries ─────────────────────────────── */

function dropHint(dropped: readonly BlockId[]): string {
  if (dropped.length === 0) return "";
  if (dropped.length === 1) return `Drops ${BLOCK_INFO[dropped[0]!].shortName}`;
  return `Drops ${dropped.length} blocks`;
}

/** Keeps a row without an icon aligned with the rows above it that have one. */
const NO_ICON = <span className={s.menuIconSpacer} />;

/** Take the region out, with a toast that can put it back. */
function removeWithUndo(how: Parameters<typeof removeSplitRegion>[0]): void {
  withUndo("Removed split", () => removeSplitRegion(how));
}

/**
 * The three explicit ways out of a split, as menu rows under a "Remove split"
 * heading: keep lane A, keep lane B, or run A then B in series. Each row names
 * what it drops, so taking a dual-amp rig apart never silently stacks two
 * amps in series (the old single "rejoin" did exactly that). Each removal
 * offers Undo.
 */
export function splitRemoveEntries(
  order: readonly BlockId[],
  names: Record<LaneSide, string>,
): MenuEntry[] {
  const droppedA = blocksDroppedBy(order, "keepB");
  const droppedB = blocksDroppedBy(order, "keepA");
  return [
    { kind: "section", id: "remove-title", label: "Remove split" },
    {
      id: "remove-keep-a",
      label: `Keep lane ${names.a}`,
      icon: NO_ICON,
      hint: dropHint(droppedB),
      destructive: droppedB.length > 0,
      onSelect: () => removeWithUndo("keepA"),
    },
    {
      id: "remove-keep-b",
      label: `Keep lane ${names.b}`,
      icon: NO_ICON,
      hint: dropHint(droppedA),
      destructive: droppedA.length > 0,
      onSelect: () => removeWithUndo("keepB"),
    },
    {
      id: "remove-series",
      label: `Run ${names.a} then ${names.b}`,
      icon: NO_ICON,
      onSelect: () => removeWithUndo("series"),
    },
  ];
}
