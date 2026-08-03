/**
 * Lane furniture: the IN / OUT terminals and the [+] node with its picker menu.
 *
 * Same three-layer shape as a block card (position → entrance → face), so the
 * whole lane enters as one staggered build-in and reflows on the same springs.
 */
import { useMemo } from "react";
import { motion } from "motion/react";
import { BLOCK_IDS, type BlockId } from "../../bridge/types";
import { BLOCK_ACCENT, BLOCK_INFO } from "../../theme/blocks";
import { Button, Menu, Tooltip, cx, useMenu, PlusIcon } from "../../components";
import type { MenuEntry } from "../../components";
import { BlockGlyph, InputGlyph, OutputGlyph } from "../../components";
import { CARD_VARIANTS, CARD_VARIANTS_REDUCED } from "./BlockCard";
import { ADD_H, ADD_W, TERMINAL_H, TERMINAL_W } from "./layout";
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

/** Vercel-style picker rows: one per absent block, tinted with its own accent. */
export function pickerEntries(
  absent: readonly BlockId[],
  onPick: (id: BlockId) => void,
): MenuEntry[] {
  const entries: MenuEntry[] = [
    { kind: "section", id: "title", label: "Add to chain" },
  ];
  for (const id of absent) {
    entries.push({
      id,
      label: BLOCK_INFO[id].displayName,
      icon: (
        <span className={s.pickerIcon} style={{ color: BLOCK_ACCENT[id] }}>
          <BlockGlyph block={id} size={16} strokeWidth={1.6} />
        </span>
      ),
      hint: BLOCK_INFO[id].shortName,
      onSelect: () => onPick(id),
    });
  }
  return entries;
}

/** Blocks not in the chain, in the frozen enum order (= the add-menu order). */
export function absentBlocks(order: readonly BlockId[]): BlockId[] {
  return BLOCK_IDS.filter((id) => !order.includes(id));
}

/* ─────────────────────────────── the [+] node ──────────────────────────── */

export interface AddNodeProps {
  node: NodeMotion;
  absent: readonly BlockId[];
  reduced: boolean;
  onAdd(id: BlockId): void;
}

export function AddNode({ node, absent, reduced, onAdd }: AddNodeProps) {
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu<HTMLButtonElement>();
  const entries = useMemo(() => pickerEntries(absent, onAdd), [absent, onAdd]);

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

/* ─────────────────────── empty-chain add affordance ────────────────────── */

export interface AddBlockButtonProps {
  absent: readonly BlockId[];
  onAdd(id: BlockId): void;
}

export function AddBlockButton({ absent, onAdd }: AddBlockButtonProps) {
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu<HTMLSpanElement>();
  const entries = useMemo(() => pickerEntries(absent, onAdd), [absent, onAdd]);
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
