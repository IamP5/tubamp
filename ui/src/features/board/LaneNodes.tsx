/**
 * Board furniture: the IN / OUT terminals and the [+] node with its picker menu.
 *
 * Same three-layer shape as a block card (position → entrance → face), so the
 * whole board enters as one staggered build-in and reflows on the same springs.
 */
import { useMemo } from "react";
import { motion } from "motion/react";
import {
  instanceTokensOfKind,
  isFxBlockId,
  isStructureBlockId,
  type BaseBlockId,
  type BlockId,
} from "../../bridge/types";
import { BLOCK_ACCENT, BLOCK_INFO } from "../../theme/blocks";
import { BLOCK_BASES, addableKinds, instancesInChain } from "../../store";
import { Button, Menu, Tooltip, cx, useMenu, PlusIcon } from "../../components";
import type { MenuEntry } from "../../components";
import { BlockGlyph, InputGlyph, OutputGlyph } from "../../components";
import { CARD_VARIANTS, CARD_VARIANTS_REDUCED } from "./BlockCard";
import {
  ADD_H,
  ADD_W,
  GAP,
  TERMINAL_H,
  TERMINAL_W,
  type InsertPoint,
} from "./layout";
import type { NodeMotion } from "./nodes";
import s from "./board.module.css";

/* ───────────────────────────── IN / OUT terminals ──────────────────────── */

export interface TerminalProps {
  kind: "in" | "out";
  node: NodeMotion;
  reduced: boolean;
}

export function Terminal({ kind, node, reduced }: TerminalProps) {
  const Glyph = kind === "in" ? InputGlyph : OutputGlyph;
  return (
    <motion.div
      className={s.node}
      style={{
        x: node.x,
        y: node.y,
        width: TERMINAL_W,
        height: TERMINAL_H,
        marginTop: -TERMINAL_H / 2,
        zIndex: 1,
      }}
      exit="exit"
    >
      <motion.div
        className={s.cardEnter}
        variants={reduced ? CARD_VARIANTS_REDUCED : CARD_VARIANTS}
      >
        <div className={s.terminal}>
          <Glyph size={22} strokeWidth={1.6} />
        </div>
        <span className={s.terminalCaption}>{kind === "in" ? "IN" : "OUT"}</span>
      </motion.div>
    </motion.div>
  );
}

/* ──────────────────────────────── block picker ─────────────────────────── */

/**
 * One picker row per block KIND, not per instance: picking "Compressor" adds the
 * lowest free instance (`addBlock`). A duplicable kind stays listed all the way
 * to 3/3 with a count hint, and greys out there rather than vanishing — a row
 * that disappears mid-session reads as a bug. Singletons (gate, amp, cab and the
 * three fx slots) are simply dropped once they are in the chain: they have no
 * count to show and nothing to say.
 */
export interface PickerKind {
  base: BaseBlockId;
  used: number;
  total: number;
  addable: boolean;
}

export function pickerKinds(order: readonly BlockId[]): PickerKind[] {
  const free = new Set<BaseBlockId>(addableKinds(order));
  const kinds: PickerKind[] = [];
  for (const base of BLOCK_BASES) {
    // Structural tokens are never picker rows: `split` costs three tokens and
    // comes in through its own entry, `lane2`/`mix` are not blocks at all.
    // `BLOCK_BASES` already leaves them out — this is the belt that also proves
    // to the compiler that what is left is an addable kind.
    if (isStructureBlockId(base)) continue;
    const total = isFxBlockId(base) ? 1 : instanceTokensOfKind(base).length;
    const addable = free.has(base);
    // Singletons carry no count, so an exhausted one has nothing to render.
    if (total === 1 && !addable) continue;
    kinds.push({ base, used: instancesInChain(base, order), total, addable });
  }
  return kinds;
}

/** True when at least one kind can still be added — half of the [+] node's gate
 *  (the other half is "a split can still be created", see the board). */
export function canAddBlock(kinds: readonly PickerKind[]): boolean {
  return kinds.some((kind) => kind.addable);
}

/**
 * The "Add split" row, when the board offers one. Bound by the caller so each
 * [+] can insert the fork at its own position. The row is absent entirely once a
 * structure exists (exactly one region per chain) and merely disabled when the
 * chain has no room for the three tokens it costs.
 */
export interface SplitOption {
  enabled: boolean;
  onSelect(): void;
}

/** Vercel-style picker rows: one per kind, tinted with its own accent, plus the
 *  structural "Add split" row under a separator — it makes a shape, not a block,
 *  and reading as one more block kind would be a lie. */
export function pickerEntries(
  kinds: readonly PickerKind[],
  onPick: (id: BlockId) => void,
  split?: SplitOption,
): MenuEntry[] {
  const entries: MenuEntry[] = [
    { kind: "section", id: "title", label: "Add to chain" },
  ];
  for (const { base, used, total, addable } of kinds) {
    entries.push({
      id: base,
      label: BLOCK_INFO[base].displayName,
      icon: (
        <span className={s.pickerIcon} style={{ color: BLOCK_ACCENT[base] }}>
          <BlockGlyph block={base} size={16} strokeWidth={1.6} />
        </span>
      ),
      hint: total > 1 ? `${used}/${total}` : BLOCK_INFO[base].shortName,
      disabled: !addable,
      onSelect: () => onPick(base),
    });
  }
  if (split) {
    entries.push({ kind: "separator", id: "split-sep" });
    entries.push({
      id: "split",
      label: "Split path",
      icon: (
        <span className={s.pickerIcon} style={{ color: BLOCK_ACCENT.split }}>
          <BlockGlyph block="split" size={16} strokeWidth={1.6} />
        </span>
      ),
      hint: "LANES",
      disabled: !split.enabled,
      onSelect: split.onSelect,
    });
  }
  return entries;
}

/* ─────────────────────────────── the [+] node ──────────────────────────── */

export interface AddNodeProps {
  node: NodeMotion;
  kinds: readonly PickerKind[];
  reduced: boolean;
  onAdd(id: BlockId): void;
  /** Appends the fork at the end of the chain; absent once one exists. */
  onAddSplit?: SplitOption;
}

export function AddNode({
  node,
  kinds,
  reduced,
  onAdd,
  onAddSplit,
}: AddNodeProps) {
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu<HTMLButtonElement>();
  const entries = useMemo(
    () => pickerEntries(kinds, onAdd, onAddSplit),
    [kinds, onAdd, onAddSplit],
  );

  return (
    <motion.div
      className={s.node}
      style={{
        x: node.x,
        y: node.y,
        width: ADD_W,
        height: ADD_H,
        marginTop: -ADD_H / 2,
        zIndex: 2,
      }}
      exit="exit"
    >
      <motion.div
        className={s.cardEnter}
        variants={reduced ? CARD_VARIANTS_REDUCED : CARD_VARIANTS}
      >
        <Tooltip label="Add a block to the chain" placement="top">
          <button
            type="button"
            ref={anchorRef}
            className={cx(s.addNode, open && s.addNodeOpen)}
            aria-label="Add a block to the chain"
            onPointerDown={(event) => event.stopPropagation()}
            onClick={toggle}
          >
            <PlusIcon size={18} />
          </button>
        </Tooltip>
      </motion.div>

      <Menu
        anchor={anchor}
        open={open}
        onClose={close}
        entries={entries}
        align="center"
        minWidth={210}
      />
    </motion.div>
  );
}

/* ─────────────────────────── mid-chain inserts ─────────────────────────── */

/** Hover hit-box straddling a connector gap. Width covers the whole gap;
 *  height gives some slack around the wire without reaching into the cards. */
const INSERT_HIT_W = GAP;
const INSERT_HIT_H = 56;

export interface InsertNodeProps {
  point: InsertPoint;
  kinds: readonly PickerKind[];
  onAdd(id: BlockId, at: { index: number; row: number }): void;
  /** Inserts the fork at THIS gap; absent once a structure exists. */
  onAddSplit?(at: { index: number; row: number }): void;
  /** False when the chain has no room for the fork's three tokens. */
  canAddSplit?: boolean;
}

/**
 * A [+] that lives in a connector gap and only shows itself on hover — the
 * "insert here" counterpart of the tail [+] node, sharing its picker. The
 * hit-box deliberately has no pointer handlers of its own: a press in a gap
 * that misses the dot must still bubble to the stage (pan / background tap).
 */
export function InsertNode({
  point,
  kinds,
  onAdd,
  onAddSplit,
  canAddSplit = false,
}: InsertNodeProps) {
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu<HTMLButtonElement>();
  const entries = useMemo(
    () =>
      pickerEntries(
        kinds,
        (id) => onAdd(id, { index: point.index, row: point.row }),
        onAddSplit
          ? {
              enabled: canAddSplit,
              onSelect: () =>
                onAddSplit({ index: point.index, row: point.row }),
            }
          : undefined,
      ),
    [canAddSplit, kinds, onAdd, onAddSplit, point.index, point.row],
  );

  return (
    <div
      className={s.insertHit}
      style={{
        left: point.x - INSERT_HIT_W / 2,
        top: point.y - INSERT_HIT_H / 2,
        width: INSERT_HIT_W,
        height: INSERT_HIT_H,
      }}
      data-open={open || undefined}
    >
      <Tooltip label="Insert a block here" placement="top">
        <button
          type="button"
          ref={anchorRef}
          className={s.insertDot}
          aria-label={`Insert a block at position ${point.index + 1}`}
          onPointerDown={(event) => event.stopPropagation()}
          onClick={toggle}
        >
          <PlusIcon size={12} />
        </button>
      </Tooltip>

      <Menu
        anchor={anchor}
        open={open}
        onClose={close}
        entries={entries}
        align="center"
        minWidth={210}
      />
    </div>
  );
}

/* ─────────────────────── empty-chain add affordance ────────────────────── */

export interface AddBlockButtonProps {
  kinds: readonly PickerKind[];
  onAdd(id: BlockId): void;
  onAddSplit?: SplitOption;
}

export function AddBlockButton({
  kinds,
  onAdd,
  onAddSplit,
}: AddBlockButtonProps) {
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu<HTMLSpanElement>();
  const entries = useMemo(
    () => pickerEntries(kinds, onAdd, onAddSplit),
    [kinds, onAdd, onAddSplit],
  );
  return (
    <>
      <span ref={anchorRef} className={s.inlineAnchor}>
        <Button
          size="sm"
          icon={<PlusIcon size={14} />}
          onPointerDown={(event) => event.stopPropagation()}
          onClick={toggle}
        >
          Add a block
        </Button>
      </span>
      <Menu
        anchor={anchor}
        open={open}
        onClose={close}
        entries={entries}
        align="center"
        minWidth={210}
      />
    </>
  );
}
