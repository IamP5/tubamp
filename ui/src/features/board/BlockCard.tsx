/**
 * A block tile on the board: ~148×48, a text tile in the manner of Helix Native
 * and Quad Cortex. The glyph on a chip in its family's hue (neutral for plugins
 * and routing, and while bypassed), the block's short name, and under it one line saying
 * what is in the block (its type, capture, IR or plugin). Power and More sit on
 * the right edge, quiet until the tile is hovered or selected; power also stays
 * whenever the block is bypassed.
 *
 * Three nested layers, on purpose:
 *   position layer — bound to the node's x/y motion values (drag / reflow write
 *                    these; nothing else may animate x or y on this element)
 *   entrance layer — owns the variant-driven enter/exit (opacity/scale), which
 *                    is what the lane's stagger container orchestrates
 *   tile layer     — the drag pick-up scale; selection and the drag shadow are
 *                    plain classes (no animated shadow)
 */
import { useMemo, type PointerEvent as ReactPointerEvent } from "react";
import { motion, type Variants } from "motion/react";
import {
  COMBO_PARAM_IDS,
  fxSlotIndexOf,
  kindOf,
  type BlockId,
  type ComboParamId,
  type FxSlotIndex,
} from "../../bridge/types";
import { useComboParam } from "../../hooks";
import { useStore } from "../../store";
import { BLOCK_INFO } from "../../theme/blocks";
import { duration, ease, tween } from "../../theme/motion";
import {
  AlertIcon,
  BlockIcon,
  IconButton,
  Menu,
  MoreIcon,
  PowerButton,
  PowerIcon,
  Tooltip,
  TrashIcon,
  cx,
  useMenu,
} from "../../components";
import type { MenuEntry } from "../../components";
import { powerLabel } from "./lanes";
import { CARD_H, CARD_W } from "./layout";
import type { NodeMotion } from "./nodes";
import s from "./board.module.css";

export const CARD_VARIANTS: Variants = {
  hidden: { opacity: 0, scale: 0.98 },
  show: { opacity: 1, scale: 1, transition: tween.enter },
  exit: { opacity: 0, scale: 0.98, transition: tween.exit },
};

export const CARD_VARIANTS_REDUCED: Variants = {
  hidden: { opacity: 0 },
  show: { opacity: 1, transition: tween.instant },
  exit: { opacity: 0, transition: tween.instant },
};

/** Pick-up and set-down of the dragged tile: a 1.02 scale, no overshoot. */
const LIFT = { duration: duration.fast, ease: ease.out };

export interface BlockCardProps {
  id: BlockId;
  node: NodeMotion;
  selected: boolean;
  enabled: boolean;
  dragging: boolean;
  reduced: boolean;
  /** The second line, when the caller knows it (Split, Mixer). Omitted: the
   *  tile reads it from the block's own state (see `BlockDetail`). */
  detail?: string;
  /** Something is wrong with this block (a missing capture or IR, a plugin
   *  that is not running): a warn icon after the name, this as its tooltip. */
  warning?: string;
  /** APVTS index of the `*_on` parameter — Logic's touch-to-select reads this. */
  paramIndex: number;
  onPointerDown(event: ReactPointerEvent<HTMLElement>, id: BlockId): void;
  onToggle(): void;
  onRemove(): void;
  /** Menu wording for the destructive entry. */
  removeLabel?: string;
  /** Extra menu rows between Bypass and the remove entry ("Make dual amp",
   *  "Swap lanes" …). */
  extraEntries?: readonly MenuEntry[];
  /** Replaces the single remove row — Split / Mixer offer the three explicit
   *  ways out of a region instead (see splitActions.tsx). */
  removeEntries?: readonly MenuEntry[];
}

/** Keeps a control's press from selecting the tile or starting a reorder. */
function swallow(event: ReactPointerEvent<HTMLElement>): void {
  event.stopPropagation();
}

export function BlockCard({
  id,
  node,
  selected,
  enabled,
  dragging,
  reduced,
  detail,
  warning,
  paramIndex,
  onPointerDown,
  onToggle,
  onRemove,
  removeLabel = "Remove",
  extraEntries,
  removeEntries,
}: BlockCardProps) {
  const info = BLOCK_INFO[id];
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu<HTMLSpanElement>();

  const entries = useMemo<MenuEntry[]>(
    () => [
      { kind: "section", id: "title", label: info.displayName },
      {
        id: "bypass",
        label: powerLabel(id, enabled),
        icon: <PowerIcon />,
        onSelect: onToggle,
      },
      ...(extraEntries ?? []),
      { kind: "separator", id: "sep" },
      ...(removeEntries ?? [
        {
          id: "remove",
          label: removeLabel,
          icon: <TrashIcon />,
          destructive: true,
          onSelect: onRemove,
        },
      ]),
    ],
    [
      enabled,
      id,
      extraEntries,
      info.displayName,
      onRemove,
      onToggle,
      removeEntries,
      removeLabel,
    ],
  );

  return (
    <motion.div
      className={s.node}
      data-block={id}
      data-family={info.family}
      role="group"
      aria-label={info.displayName}
      style={{
        x: node.x,
        y: node.y,
        width: CARD_W,
        height: CARD_H,
        marginTop: -CARD_H / 2,
        zIndex: dragging ? 30 : 2,
      }}
      exit="exit"
    >
      <motion.div
        className={s.cardEnter}
        variants={reduced ? CARD_VARIANTS_REDUCED : CARD_VARIANTS}
      >
        <motion.div
          className={cx(
            s.card,
            selected && s.cardSelected,
            !enabled && s.cardBypassed,
            dragging && s.cardDragging,
            open && s.cardMenuOpen,
          )}
          animate={{ scale: dragging && !reduced ? 1.02 : 1 }}
          transition={LIFT}
          onPointerDown={(event) => onPointerDown(event, id)}
          onDoubleClick={onToggle}
        >
          <span className={s.cardIcon}>
            <BlockIcon block={id} size={16} strokeWidth={1.5} />
          </span>

          <div className={s.cardText}>
            <span className={s.cardNameRow}>
              <span className={s.cardName}>{info.shortName}</span>
              {warning && (
                <Tooltip label={warning} placement="top" className={s.cardWarn}>
                  <AlertIcon size={12} strokeWidth={1.75} aria-label={warning} />
                </Tooltip>
              )}
            </span>
            {detail === undefined ? (
              <BlockDetail id={id} />
            ) : (
              <DetailLine text={detail} />
            )}
          </div>

          <div className={s.cardActions}>
            <span ref={anchorRef} className={s.inlineAnchor}>
              <IconButton
                size="sm"
                className={s.cardReveal}
                aria-label={`${info.displayName} options`}
                aria-expanded={open}
                onPointerDown={swallow}
                onClick={toggle}
                onDoubleClick={(event) => event.stopPropagation()}
              >
                <MoreIcon />
              </IconButton>
            </span>
            {/* The span keeps a press on the power button from starting a drag
                and a double-click from toggling twice; the kit button already
                stops its click. */}
            <span
              className={s.inlineAnchor}
              onPointerDown={swallow}
              onDoubleClick={(event) => event.stopPropagation()}
            >
              <Tooltip label={powerLabel(id, enabled)} placement="top">
                <PowerButton
                  size="sm"
                  on={enabled}
                  onChange={onToggle}
                  className={cx(s.cardReveal, s.cardRevealPower)}
                  data-param-index={paramIndex}
                  aria-label={`${info.displayName} power`}
                />
              </Tooltip>
            </span>
          </div>
        </motion.div>
      </motion.div>

      <Menu
        anchor={anchor}
        open={open}
        onClose={close}
        entries={entries}
        align="center"
        minWidth={removeEntries ? 220 : 180}
      />
    </motion.div>
  );
}

/* ─────────────────────────────── second line ───────────────────────────── */

/** One truncated line; the tooltip carries the whole text. Nothing at all for
 *  an empty string, so the name centres on its own. */
function DetailLine({ text }: { text: string }) {
  if (!text) return null;
  return (
    <Tooltip label={text} placement="bottom" className={s.cardDetailAnchor}>
      <span className={s.cardDetail}>{text}</span>
    </Tooltip>
  );
}

/** The combo that names a typed block's current engine: `<token>_type`, or the
 *  gate's `gate_mode`. Null for blocks without one. */
function typeParamOf(id: BlockId): ComboParamId | null {
  const name = kindOf(id) === "gate" ? "gate_mode" : `${id}_type`;
  return (COMBO_PARAM_IDS as readonly string[]).includes(name)
    ? (name as ComboParamId)
    : null;
}

/**
 * What is in the block, read from state that already exists: the type for
 * typed blocks, the capture for Amp / Amp 2, the IR for Cab, the plugin for a
 * Plugin slot. Each source is its own component so a tile subscribes to exactly
 * the one it shows.
 */
function BlockDetail({ id }: { id: BlockId }) {
  const param = typeParamOf(id);
  if (param) return <TypeDetail param={param} />;
  if (id === "amp" || id === "amp2") return <CaptureDetail engineB={id === "amp2"} />;
  if (id === "cab") return <IrDetail />;
  const slot = fxSlotIndexOf(id);
  if (slot !== -1) return <PluginDetail slot={slot} />;
  return null;
}

function TypeDetail({ param }: { param: ComboParamId }) {
  return <DetailLine text={useComboParam(param).text} />;
}

function CaptureDetail({ engineB }: { engineB: boolean }) {
  const model = useStore((st) => (engineB ? st.modelB : st.model));
  return <DetailLine text={model ? model.tone?.title || model.name : ""} />;
}

function IrDetail() {
  const ir = useStore((st) => st.ir);
  return <DetailLine text={ir ? ir.tone?.title || ir.name : ""} />;
}

function PluginDetail({ slot }: { slot: FxSlotIndex }) {
  const state = useStore((st) => st.fxSlots[slot]);
  if (!state?.occupied) return null;
  return <DetailLine text={state.loading ? "Loading" : state.name} />;
}
