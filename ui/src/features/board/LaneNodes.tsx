/**
 * Board furniture: the In / Out end caps, the add tile with its picker, the
 * hover inserts on the wires, and the split region's lane furniture — each
 * lane's label and its own add slot.
 *
 * Same three-layer shape as a block tile (position → entrance → face), so the
 * whole board enters as one staggered build-in and reflows on the same springs.
 */
import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from "react";
import { createPortal } from "react-dom";
import { AnimatePresence, motion } from "motion/react";
import {
  isStructureBlockId,
  type BaseBlockId,
  type BlockId,
} from "../../bridge/types";
import { BLOCK_INFO } from "../../theme/blocks";
import { duration, ease } from "../../theme/motion";
import { BLOCK_BASES, addableKinds } from "../../store";
import {
  BlockIcon,
  Button,
  PlusIcon,
  Scrim,
  Tooltip,
  cx,
  kit,
  useMenu,
} from "../../components";
import { CARD_VARIANTS, CARD_VARIANTS_REDUCED } from "./BlockCard";
import {
  ADD_H,
  ADD_W,
  CARD_H,
  GAP,
  LANE_ADD_H,
  LANE_ADD_W,
  TERMINAL_H,
  TERMINAL_W,
  type InsertPoint,
} from "./layout";
import type { LaneSide } from "./lanes";
import type { NodeMotion } from "./nodes";
import s from "./board.module.css";

/* ─────────────────────────────── In / Out caps ─────────────────────────── */

export interface TerminalProps {
  kind: "in" | "out";
  node: NodeMotion;
  reduced: boolean;
}

/** "In" / "Out": the word at the wire's end, nothing else. */
export function Terminal({ kind, node, reduced }: TerminalProps) {
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
        <span
          className={cx(s.terminal, kind === "in" ? s.terminalIn : s.terminalOut)}
        >
          {kind === "in" ? "In" : "Out"}
        </span>
      </motion.div>
    </motion.div>
  );
}

/* ──────────────────────────────── block picker ─────────────────────────── */

/**
 * One picker row per block KIND, not per instance: picking "Compressor" adds
 * the lowest free instance (`addBlock`). Every kind is always listed, in its
 * section; one with no free instance (or a full chain) is disabled rather
 * than dropped, so the picker never changes shape under the user.
 */
export interface PickerKind {
  base: BaseBlockId;
  addable: boolean;
}

export function pickerKinds(order: readonly BlockId[]): PickerKind[] {
  const free = new Set<BaseBlockId>(addableKinds(order));
  const kinds: PickerKind[] = [];
  for (const base of BLOCK_BASES) {
    // Structural tokens are never picker rows: `split` costs three tokens and
    // comes in through its own entry, `lane2`/`mix` are not blocks at all.
    if (isStructureBlockId(base)) continue;
    kinds.push({ base, addable: free.has(base) });
  }
  return kinds;
}

/** True when at least one kind can still be added — half of the add tile's
 *  gate (the other half is "a split can still be created", see the board). */
export function canAddBlock(kinds: readonly PickerKind[]): boolean {
  return kinds.some((kind) => kind.addable);
}

/**
 * The "Routing" section, when the board offers one. Bound by the caller so
 * each add control can insert at its own position. Absent entirely once a
 * structure exists (exactly one region per chain); each row is merely disabled
 * when the chain has no room for the tokens it costs.
 *
 *   Split     — `split, lane2, mix` (an empty fork).
 *   Dual amp  — wraps the chain's amp (or inserts one) with `amp2` in the
 *               other lane, hard-panned, ready for a second capture.
 */
export interface SplitOption {
  enabled: boolean;
  onSelect(): void;
  dualAmp?: { enabled: boolean; onSelect(): void };
}

interface PickerRow {
  id: string;
  label: string;
  glyph: BlockId;
  disabled: boolean;
  onSelect(): void;
}

interface PickerSection {
  id: string;
  label: string;
  rows: PickerRow[];
}

/** The picker's sections, in reading order, split over two columns. */
const SECTIONS: readonly { id: string; label: string; kinds: BaseBlockId[] }[] = [
  { id: "dynamics", label: "Dynamics", kinds: ["gate", "comp"] },
  { id: "gain", label: "Gain", kinds: ["drive", "amp", "amp2"] },
  { id: "tone", label: "Tone", kinds: ["cab", "eq"] },
  { id: "time", label: "Modulation and time", kinds: ["mod", "delay", "reverb"] },
  { id: "plugins", label: "Plugins", kinds: ["fx1", "fx2", "fx3"] },
];
/** Sections before this index fill the first column. */
const FIRST_COLUMN = 3;

function pickerSections(
  kinds: readonly PickerKind[],
  onPick: (id: BlockId) => void,
  split?: SplitOption,
): PickerSection[] {
  const byBase = new Map(kinds.map((kind) => [kind.base, kind]));
  const sections: PickerSection[] = SECTIONS.map((section) => ({
    id: section.id,
    label: section.label,
    rows: section.kinds.flatMap((base) => {
      const kind = byBase.get(base);
      if (!kind) return [];
      return [
        {
          id: base,
          label: BLOCK_INFO[base].displayName,
          glyph: base,
          disabled: !kind.addable,
          onSelect: () => onPick(base),
        },
      ];
    }),
  }));
  if (split) {
    const rows: PickerRow[] = [
      {
        id: "split",
        label: "Split",
        glyph: "split",
        disabled: !split.enabled,
        onSelect: split.onSelect,
      },
    ];
    if (split.dualAmp)
      rows.push({
        id: "split-dual-amp",
        label: "Dual amp",
        glyph: "amp2",
        disabled: !split.dualAmp.enabled,
        onSelect: split.dualAmp.onSelect,
      });
    sections.push({ id: "routing", label: "Routing", rows });
  }
  return sections;
}

interface PickerMenuProps {
  anchor: HTMLElement | null;
  open: boolean;
  onClose(): void;
  kinds: readonly PickerKind[];
  onPick(id: BlockId): void;
  split?: SplitOption;
}

/**
 * The block picker: the kit's menu surface and rows (`kit.menu`,
 * `kit.menuItem`, `kit.menuSectionLabel`), laid out in two columns so every
 * section fits a 600px window without scrolling — the kit `Menu` is one
 * column. Placement follows `Menu`: below the anchor, above it when there is
 * no room, clamped inside the window.
 */
function PickerMenu({ anchor, open, onClose, kinds, onPick, split }: PickerMenuProps) {
  const menuRef = useRef<HTMLDivElement>(null);
  const [pos, setPos] = useState({ top: 0, left: 0 });
  const sections = useMemo(
    () => pickerSections(kinds, onPick, split),
    [kinds, onPick, split],
  );
  const columns = [sections.slice(0, FIRST_COLUMN), sections.slice(FIRST_COLUMN)];

  const place = useCallback(() => {
    if (!anchor || !menuRef.current) return;
    const a = anchor.getBoundingClientRect();
    const m = menuRef.current.getBoundingClientRect();
    let left = a.left + (a.width - m.width) / 2;
    let top = a.bottom + 6;
    if (top + m.height > window.innerHeight - 8) {
      top = Math.max(8, a.top - m.height - 6);
    }
    left = Math.min(Math.max(8, left), window.innerWidth - m.width - 8);
    setPos({ top, left });
  }, [anchor]);

  useLayoutEffect(() => {
    if (open) place();
  }, [open, place, sections.length]);

  useEffect(() => {
    if (!open) return;
    const onKey = (event: KeyboardEvent): void => {
      if (event.key === "Escape") onClose();
    };
    window.addEventListener("keydown", onKey);
    window.addEventListener("resize", place);
    return () => {
      window.removeEventListener("keydown", onKey);
      window.removeEventListener("resize", place);
    };
  }, [open, onClose, place]);

  return createPortal(
    <AnimatePresence>
      {open && (
        <>
          <Scrim className={kit.menuScrim} onPointerDown={onClose} />
          <motion.div
            ref={menuRef}
            role="menu"
            className={cx(kit.menu, s.picker)}
            style={{ top: pos.top, left: pos.left }}
            initial={{ opacity: 0, y: -4 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: -4 }}
            transition={{ duration: duration.fast, ease: ease.out }}
          >
            {columns.map((column, index) => (
              <div key={index} className={s.pickerColumn}>
                {column.map((section) => (
                  <div key={section.id} role="group" aria-label={section.label}>
                    <div className={kit.menuSectionLabel}>{section.label}</div>
                    {section.rows.map((row) => (
                      <button
                        key={row.id}
                        type="button"
                        role="menuitem"
                        disabled={row.disabled}
                        data-family={BLOCK_INFO[row.glyph].family}
                        className={cx(kit.menuItem, s.pickerRow)}
                        onClick={() => {
                          row.onSelect();
                          onClose();
                        }}
                      >
                        <span className={cx(kit.menuItemIcon, s.pickerIcon)}>
                          <BlockIcon block={row.glyph} size={16} strokeWidth={1.5} />
                        </span>
                        <span className={kit.menuItemLabel}>{row.label}</span>
                      </button>
                    ))}
                  </div>
                ))}
              </div>
            ))}
          </motion.div>
        </>
      )}
    </AnimatePresence>,
    document.body,
  );
}

/* ─────────────────────────────── the add tile ──────────────────────────── */

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
        <Tooltip label="Add block" placement="top">
          <button
            type="button"
            ref={anchorRef}
            className={cx(s.addTile, open && s.addTileOpen)}
            aria-label="Add a block to the chain"
            aria-expanded={open}
            onPointerDown={(event) => event.stopPropagation()}
            onClick={toggle}
          >
            <PlusIcon />
          </button>
        </Tooltip>
      </motion.div>

      <PickerMenu
        anchor={anchor}
        open={open}
        onClose={close}
        kinds={kinds}
        onPick={onAdd}
        split={onAddSplit}
      />
    </motion.div>
  );
}

/* ─────────────────────────── mid-chain inserts ─────────────────────────── */

/** Hover hit-box straddling a wire gap: the whole gap wide, a tile tall. */
const INSERT_HIT_W = GAP;
const INSERT_HIT_H = CARD_H;

export interface InsertNodeProps {
  point: InsertPoint;
  kinds: readonly PickerKind[];
  onAdd(id: BlockId, at: { index: number; row: number }): void;
  /** The "Routing" section bound to THIS gap; absent once a structure exists
   *  (and inside a lane, where a second region could never go). */
  splitFor?(at: { index: number; row: number }): SplitOption | undefined;
}

/**
 * A round "+" that lives in a wire gap and only shows itself on hover — the
 * "insert here" counterpart of the add tile, sharing its picker. The hit-box
 * deliberately has no pointer handlers of its own: a press in a gap that
 * misses the button must still bubble to the stage (pan / background tap).
 */
export function InsertNode({ point, kinds, onAdd, splitFor }: InsertNodeProps) {
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu<HTMLButtonElement>();
  const at = useMemo(
    () => ({ index: point.index, row: point.row }),
    [point.index, point.row],
  );
  const pick = useCallback((id: BlockId) => onAdd(id, at), [at, onAdd]);
  const split = useMemo(() => splitFor?.(at), [at, splitFor]);

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
      <Tooltip label="Insert block" placement="top">
        <button
          type="button"
          ref={anchorRef}
          className={cx(s.roundButton, s.insertButton, open && s.roundButtonOpen)}
          aria-label={`Insert a block at position ${point.index + 1}`}
          aria-expanded={open}
          onPointerDown={(event) => event.stopPropagation()}
          onClick={toggle}
        >
          <span className={s.roundMark}>
            <PlusIcon size={12} />
          </span>
        </button>
      </Tooltip>

      <PickerMenu
        anchor={anchor}
        open={open}
        onClose={close}
        kinds={kinds}
        onPick={pick}
        split={split}
      />
    </div>
  );
}

/* ─────────────────────── empty-chain add action ────────────────────────── */

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
  return (
    <>
      <span ref={anchorRef} className={s.inlineAnchor}>
        <Button
          icon={<PlusIcon />}
          aria-label="Add a block to the chain"
          aria-expanded={open}
          onPointerDown={(event) => event.stopPropagation()}
          onClick={toggle}
        >
          Add block
        </Button>
      </span>
      <PickerMenu
        anchor={anchor}
        open={open}
        onClose={close}
        kinds={kinds}
        onPick={onAdd}
        split={onAddSplit}
      />
    </>
  );
}

/* ──────────────────────────── lane furniture ───────────────────────────── */

export interface LaneAddNodeProps {
  side: LaneSide;
  /** The lane's name ("A", "L", "Low" …). */
  name: string;
  node: NodeMotion;
  kinds: readonly PickerKind[];
  reduced: boolean;
  /** Appends `id` to the end of THIS lane. */
  onAdd(id: BlockId): void;
}

/**
 * The lane's own add slot: a small round "+" closing the lane, always visible,
 * so an empty lane reads as a place to put something and filling it is one
 * click. It is wired like the add tile on the main path (last tile → slot →
 * Mixer), and it is a drop target too — dragging a tile onto it appends to
 * the lane.
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
            className={cx(s.roundButton, s.laneAdd, open && s.roundButtonOpen)}
            data-lane={side}
            aria-label={`Add a block to lane ${name}`}
            aria-expanded={open}
            onPointerDown={(event) => event.stopPropagation()}
            onClick={toggle}
          >
            <span className={s.roundMark}>
              <PlusIcon size={12} />
            </span>
          </button>
        </Tooltip>
      </motion.div>

      <PickerMenu
        anchor={anchor}
        open={open}
        onClose={close}
        kinds={kinds}
        onPick={onAdd}
      />
    </motion.div>
  );
}

export interface LaneLabelProps {
  side: LaneSide;
  name: string;
  /** What the lane receives, as a sentence (tooltip). */
  hint: string;
  /** Left edge and centre line, in lane units. Static: lanes do not move
   *  vertically, and the first lane column never moves horizontally. */
  x: number;
  y: number;
}

/** The lane's name, as plain text above its first tile. */
export function LaneLabel({ side, name, hint, x, y }: LaneLabelProps) {
  return (
    <div className={s.laneLabelAnchor} style={{ left: x, top: y }}>
      <Tooltip label={hint} placement="top">
        <span className={s.laneLabel} data-lane={side} aria-label={hint}>
          {name}
        </span>
      </Tooltip>
    </div>
  );
}
