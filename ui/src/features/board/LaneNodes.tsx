/**
 * Board furniture: the IN / OUT terminals, the [+] node with its picker menu,
 * and the split region's lane furniture — each lane's head badge and its own
 * lane-scoped [+] slot.
 *
 * Same three-layer shape as a block card (position → entrance → face), so the
 * whole board enters as one staggered build-in and reflows on the same springs.
 */
import { useMemo, type CSSProperties } from "react";
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
  LANE_ADD_H,
  LANE_ADD_W,
  TERMINAL_H,
  TERMINAL_W,
  type InsertPoint,
} from "./layout";
import { LANE_TINT, type LaneSide } from "./lanes";
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
 * The "Split path" section, when the board offers one. Bound by the caller so
 * each [+] can insert at its own position. The section is absent entirely once
 * a structure exists (exactly one region per chain); each row is merely
 * disabled when the chain has no room for the tokens it costs.
 *
 *   Empty split       — `split, lane2, mix` (an empty fork).
 *   Dual amp (stereo) — wraps the chain's amp (or inserts one) with `amp2` in
 *                       the other lane, hard-panned, ready for a 2nd capture.
 */
export interface SplitOption {
  enabled: boolean;
  onSelect(): void;
  dualAmp?: { enabled: boolean; hint: string; onSelect(): void };
}

/** Vercel-style picker rows: one per kind, tinted with its own accent, plus the
 *  structural "Split path" section under a separator — it makes a shape, not a
 *  block, and reading as one more block kind would be a lie. */
export function pickerEntries(
  kinds: readonly PickerKind[],
  onPick: (id: BlockId) => void,
  split?: SplitOption,
  title = "Add to chain",
): MenuEntry[] {
  const entries: MenuEntry[] = [{ kind: "section", id: "title", label: title }];
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
    entries.push({ kind: "section", id: "split-title", label: "Split path" });
    entries.push({
      id: "split",
      label: "Empty split",
      icon: (
        <span className={s.pickerIcon} style={{ color: BLOCK_ACCENT.split }}>
          <BlockGlyph block="split" size={16} strokeWidth={1.6} />
        </span>
      ),
      hint: "2 LANES",
      disabled: !split.enabled,
      onSelect: split.onSelect,
    });
    if (split.dualAmp) {
      entries.push({
        id: "split-dual-amp",
        label: "Dual amp (stereo)",
        icon: (
          <span className={s.pickerIcon} style={{ color: BLOCK_ACCENT.amp2 }}>
            <BlockGlyph block="amp2" size={16} strokeWidth={1.6} />
          </span>
        ),
        hint: split.dualAmp.hint,
        disabled: !split.dualAmp.enabled,
        onSelect: split.dualAmp.onSelect,
      });
    }
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
  /** The "Split path" section bound to THIS gap; absent once a structure
   *  exists (and inside a lane, where a second region could never go). */
  splitFor?(at: { index: number; row: number }): SplitOption | undefined;
}

/**
 * A [+] that lives in a connector gap and only shows itself on hover — the
 * "insert here" counterpart of the tail [+] node, sharing its picker. The
 * hit-box deliberately has no pointer handlers of its own: a press in a gap
 * that misses the dot must still bubble to the stage (pan / background tap).
 */
export function InsertNode({ point, kinds, onAdd, splitFor }: InsertNodeProps) {
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu<HTMLButtonElement>();
  const entries = useMemo(() => {
    const at = { index: point.index, row: point.row };
    return pickerEntries(kinds, (id) => onAdd(id, at), splitFor?.(at));
  }, [kinds, onAdd, point.index, point.row, splitFor]);

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

/* ──────────────────────────── lane furniture ───────────────────────────── */

export interface LaneAddNodeProps {
  side: LaneSide;
  /** Badge text for this lane ("A", "L", "LOW" …). */
  name: string;
  node: NodeMotion;
  kinds: readonly PickerKind[];
  reduced: boolean;
  /** Appends `id` to the end of THIS lane. */
  onAdd(id: BlockId): void;
}

/**
 * The lane's own [+]: a small dashed slot closing the lane, always visible, so
 * an empty lane reads as a place to put something and filling it is one click.
 * It is wired like the tail [+] on the main path (last card → slot → MIX), and
 * it is a drop target too — dragging a card onto it appends to the lane.
 */
export function LaneAddNode({
  side,
  name,
  node,
  kinds,
  reduced,
  onAdd,
}: LaneAddNodeProps) {
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu<HTMLButtonElement>();
  const entries = useMemo(
    () => pickerEntries(kinds, onAdd, undefined, `Add to lane ${name}`),
    [kinds, name, onAdd],
  );
  const tint = LANE_TINT[side];
  const label = `Add a block to lane ${name}`;

  return (
    <motion.div
      className={s.node}
      style={{
        x: node.x,
        y: node.y,
        width: LANE_ADD_W,
        height: LANE_ADD_H,
        marginTop: -LANE_ADD_H / 2,
        zIndex: 2,
      }}
      exit="exit"
    >
      <motion.div
        className={s.cardEnter}
        variants={reduced ? CARD_VARIANTS_REDUCED : CARD_VARIANTS}
      >
        <Tooltip label={`Add to lane ${name}`} placement="top">
          <button
            type="button"
            ref={anchorRef}
            className={cx(s.laneAdd, open && s.laneAddOpen)}
            style={
              {
                "--lane-ink": tint.ink,
                "--lane-ring": tint.ring,
                "--lane-fill": tint.fill,
              } as CSSProperties
            }
            data-lane={side}
            aria-label={label}
            onPointerDown={(event) => event.stopPropagation()}
            onClick={toggle}
          >
            <PlusIcon size={14} />
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

export interface LaneBadgeProps {
  side: LaneSide;
  name: string;
  /** What the lane receives, as a sentence (tooltip). */
  hint: string;
  /** Left edge and centre line, in lane units. Static: the badge sits on the
   *  container frame, which does not move vertically, at the first lane column,
   *  which never moves horizontally. */
  x: number;
  y: number;
}

/**
 * The lane's head badge, set into the container frame like a legend — lane A's
 * on the top edge above its track, lane B's on the bottom edge below its own —
 * so both lanes are labelled whether or not anything is in them yet.
 */
export function LaneBadge({ side, name, hint, x, y }: LaneBadgeProps) {
  const tint = LANE_TINT[side];
  return (
    <div className={s.laneBadgeAnchor} style={{ left: x, top: y }}>
      <Tooltip label={hint} placement={side === "a" ? "top" : "bottom"}>
        <span
          className={s.laneBadge}
          data-lane={side}
          /* The tint is layered as an image so the class's opaque background
             colour stays underneath it: the badge sits on the region's dashed
             frame, and a translucent fill alone lets the dashes through. */
          style={{
            color: tint.ink,
            backgroundImage: `linear-gradient(${tint.fill}, ${tint.fill})`,
            boxShadow: `inset 0 0 0 1px ${tint.ring}`,
          }}
          aria-label={hint}
        >
          <span className={s.laneBadgeDot} style={{ background: tint.ink }} />
          {name}
        </span>
      </Tooltip>
    </div>
  );
}
