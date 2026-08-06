/**
 * A block card on the lane: ~112×80, glyph + short name, power LED, kebab menu.
 *
 * Three nested layers, on purpose:
 *   position layer — bound to the node's x/y motion values (drag / reflow write
 *                    these; nothing else may animate x or y on this element)
 *   entrance layer — owns the variant-driven enter/exit (opacity/scale/y), which
 *                    is what the lane's stagger container orchestrates
 *   lift layer     — the drag pick-up scale, plus the pre-baked shadow/ring
 *                    layers that cross-fade by opacity (never an animated
 *                    box-shadow, guardrail §4.2)
 */
import { useMemo, type PointerEvent as ReactPointerEvent } from "react";
import { motion, type Variants } from "motion/react";
import type { BlockId } from "../../bridge/types";
import { BLOCK_INFO } from "../../theme/blocks";
import { spring, tween } from "../../theme/motion";
import {
  AlertIcon,
  Menu,
  Tooltip,
  cx,
  useMenu,
  MoreIcon,
  TrashIcon,
} from "../../components";
import type { MenuEntry } from "../../components";
import { BlockGlyph, PowerGlyph } from "../../components";
import { CARD_H, CARD_W } from "./layout";
import type { NodeMotion } from "./nodes";
import s from "./board.module.css";

export const CARD_VARIANTS: Variants = {
  hidden: { opacity: 0, scale: 0.85, y: 6 },
  show: { opacity: 1, scale: 1, y: 0, transition: spring.pickup },
  exit: { opacity: 0, scale: 0.85, y: 6, transition: tween.exit },
};

export const CARD_VARIANTS_REDUCED: Variants = {
  hidden: { opacity: 0 },
  show: { opacity: 1, transition: tween.instant },
  exit: { opacity: 0, transition: tween.instant },
};

/**
 * A standing remark about the card, shown as a pill under its name: the full-rig
 * pair (the amp card states that its capture carries a cab, the cab card warns
 * that it is stacking a second one) and, on an fx slot, the hosted plugin's name.
 * Purely informational — a note never changes what the block does.
 */
export interface CardNote {
  label: string;
  tip: string;
  /** "warn" adds the alert glyph and an amber ring around the whole card. */
  tone: "info" | "warn";
}

export interface BlockCardProps {
  id: BlockId;
  node: NodeMotion;
  selected: boolean;
  enabled: boolean;
  dragging: boolean;
  reduced: boolean;
  note?: CardNote;
  /** APVTS index of the `*_on` parameter — Logic's touch-to-select reads this. */
  paramIndex: number;
  onPointerDown(event: ReactPointerEvent<HTMLElement>, id: BlockId): void;
  onToggle(): void;
  onRemove(): void;
  /** Kebab wording for the destructive entry. SPLIT/MIX say what removing them
   *  really does — it takes the whole region with it, not one tile. */
  removeLabel?: string;
}

/** Keeps a control's press from selecting the card or starting a reorder. */
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
  note,
  paramIndex,
  onPointerDown,
  onToggle,
  onRemove,
  removeLabel = "Remove from chain",
}: BlockCardProps) {
  const info = BLOCK_INFO[id];
  const { ref: anchorRef, anchor, open, toggle, close } = useMenu<HTMLButtonElement>();

  const entries = useMemo<MenuEntry[]>(
    () => [
      { kind: "section", id: "title", label: info.displayName },
      {
        id: "bypass",
        label: enabled ? "Bypass" : "Enable",
        icon: <PowerGlyph size={14} strokeWidth={1.6} />,
        onSelect: onToggle,
      },
      { kind: "separator", id: "sep" },
      {
        id: "remove",
        label: removeLabel,
        icon: <TrashIcon size={14} />,
        destructive: true,
        onSelect: onRemove,
      },
    ],
    [enabled, info.displayName, onRemove, onToggle, removeLabel],
  );

  return (
    <motion.div
      className={s.node}
      data-block={id}
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
            !enabled && s.cardBypassed,
            dragging && s.cardDragging,
          )}
          animate={{ scale: dragging ? 1.04 : 1 }}
          transition={dragging ? spring.pickup : spring.settle}
          onPointerDown={(event) => onPointerDown(event, id)}
          onDoubleClick={onToggle}
        >
          {/* Pre-baked shadow / ring layers: only their opacity ever animates. */}
          <div className={s.cardFace} />
          {/* Below the selection ring on purpose — selecting a warned card must
              still read as selected. */}
          {note?.tone === "warn" && <div className={s.cardWarnRing} />}
          <motion.div
            className={s.cardRing}
            initial={false}
            animate={{ opacity: selected ? 1 : 0 }}
            transition={tween.instant}
          />
          <motion.div
            className={s.cardLiftShadow}
            initial={false}
            animate={{ opacity: dragging ? 1 : 0 }}
            transition={tween.instant}
          />

          <div className={s.cardContent}>
            <BlockGlyph block={id} size={30} className={s.cardIcon} />
            <span className={s.cardName}>{info.shortName}</span>
          </div>

          {note && (
            <Tooltip
              label={note.tip}
              placement="bottom"
              className={s.cardNoteAnchor}
            >
              <span
                className={cx(
                  s.cardNote,
                  note.tone === "warn" && s.cardNoteWarn,
                )}
              >
                {note.tone === "warn" && (
                  <AlertIcon size={10} strokeWidth={1.8} />
                )}
                <span className={s.cardNoteText}>{note.label}</span>
              </span>
            </Tooltip>
          )}

          <Tooltip label={enabled ? "Bypass" : "Enable"} placement="top">
            <button
              type="button"
              className={s.led}
              data-param-index={paramIndex}
              aria-label={`${info.displayName} power`}
              aria-pressed={enabled}
              onPointerDown={swallow}
              onClick={onToggle}
              onDoubleClick={(event) => event.stopPropagation()}
            >
              <span className={s.ledDot} />
            </button>
          </Tooltip>

          <button
            type="button"
            ref={anchorRef}
            className={cx(s.kebab, open && s.kebabOpen)}
            aria-label={`${info.displayName} options`}
            onPointerDown={swallow}
            onClick={toggle}
            onDoubleClick={(event) => event.stopPropagation()}
          >
            <MoreIcon size={14} />
          </button>
        </motion.div>
      </motion.div>

      <Menu
        anchor={anchor}
        open={open}
        onClose={close}
        entries={entries}
        align="center"
        minWidth={190}
      />
    </motion.div>
  );
}
