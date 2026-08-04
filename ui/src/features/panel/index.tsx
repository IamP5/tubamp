/**
 * The param dock: the selected block's editor, docked over the board's bottom
 * edge (docs/REACT-UI.md §UX structure, current-ui-inventory.md
 * §BlockParamPanel).
 *
 * Structure: an accent-tinted header (icon, display name, power pill bound to
 * the block's `*_on` param, remove-from-chain affordance) over a body that
 * switches per block type. Selection is UI-local store state; with nothing
 * selected the dock is not rendered at all — the board's empty state covers it.
 *
 * Motion (motion-design.md §2.6): the dock itself slides in once, and the body
 * content-morphs between blocks inside `AnimatePresence mode="popLayout"` —
 * pure opacity, no travel, so knob positions stay visually stable. The header
 * accent underline is a `layoutId` shared element, so it slides and recolours
 * instead of remounting.
 */
import { AnimatePresence, motion, useReducedMotion } from "motion/react";
import {
  BlockIcon,
  CloseIcon,
  IconButton,
  PowerPill,
  Tooltip,
  TrashIcon,
} from "../../components";
import { useToggleParam } from "../../hooks";
import { selectFxSlotFor, useStore } from "../../store";
import { BLOCK_INFO } from "../../theme/blocks";
import { duration, ease, spring } from "../../theme/motion";
import { fxSlotIndexOf, kindOf, type BlockId } from "../../bridge";
import { AmpBody } from "./AmpBody";
import { CabBody } from "./CabBody";
import { FxSlotBody } from "./FxSlotBody";
import { ModBody } from "./ModBody";
import { KnobRow } from "./KnobRow";
import { KNOB_SPECS } from "./knobSpecs";
import { ToneModal } from "./ToneModal";
import s from "./panel.module.css";

export function Panel() {
  const selected = useStore((st) => st.selected);
  const reduced = useReducedMotion() ?? false;

  return (
    <>
      <AnimatePresence initial={false}>
        {selected !== null && (
          <motion.div
            key="dock"
            className={s.dock}
            data-block={selected}
            initial={reduced ? { opacity: 0 } : { opacity: 0, y: 28 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{
              opacity: 0,
              y: reduced ? 0 : 16,
              transition: { duration: duration.fast, ease: ease.in },
            }}
            transition={reduced ? { duration: duration.fast } : spring.ui}
          >
            <PanelHeader block={selected} />

            <div className={s.body}>
              <AnimatePresence mode="popLayout" initial={false}>
                <motion.div
                  key={selected}
                  className={s.bodyInner}
                  initial={{ opacity: 0 }}
                  animate={{ opacity: 1 }}
                  exit={{ opacity: 0 }}
                  transition={{
                    duration: reduced ? 0 : duration.fast,
                    ease: ease.out,
                  }}
                >
                  <BlockBody block={selected} />
                </motion.div>
              </AnimatePresence>
            </div>
          </motion.div>
        )}
      </AnimatePresence>

      {/* Lives outside the dock: a TONE3000 download must not depend on which
          block (if any) is selected. */}
      <ToneModal />
    </>
  );
}

/* ─────────────────────────────── header ────────────────────────────────── */

function PanelHeader({ block }: { block: BlockId }) {
  const info = BLOCK_INFO[block];
  const power = useToggleParam(info.enableParamId);
  const removeBlock = useStore((st) => st.removeBlock);
  const selectBlock = useStore((st) => st.selectBlock);
  const fxSlot = useStore(selectFxSlotFor(block));

  // A loaded slot is known by what is in it, not by which slot it is — the slot
  // number moves to a tag beside the name so both stay visible.
  const loadedName = fxSlot?.occupied ? fxSlot.name : "";

  return (
    <header className={s.header}>
      <span className={s.headerIcon}>
        <BlockIcon block={block} size={20} />
      </span>

      <div className={s.titleWrap}>
        <span className={s.titleLine}>
          <span className={s.title} title={loadedName || undefined}>
            {loadedName || info.displayName}
          </span>
          {loadedName && (
            <span className={s.headerSlotTag}>{info.shortName}</span>
          )}
        </span>
        <motion.span
          layoutId="panel-accent"
          className={s.underline}
          transition={spring.ui}
        />
      </div>

      <div className={s.headerActions} data-param-index={power.parameterIndex}>
        <Tooltip label="Enable or bypass this block">
          <PowerPill on={power.value} onChange={power.setValue} />
        </Tooltip>
        <Tooltip label="Remove this block from the chain">
          <IconButton
            aria-label="Remove this block from the chain"
            destructive
            onClick={() => removeBlock(block)}
          >
            <TrashIcon />
          </IconButton>
        </Tooltip>
        {/* Explicit dismiss: outside-click closes the panel too, but it must
            not be the only way (critique — panning/fit share that gesture). */}
        <Tooltip label="Close">
          <IconButton aria-label="Close panel" onClick={() => selectBlock(null)}>
            <CloseIcon />
          </IconButton>
        </Tooltip>
      </div>
    </header>
  );
}

/* ──────────────────────────── body dispatch ────────────────────────────── */

function BlockBody({ block }: { block: BlockId }) {
  // Keyed by KIND, not token: mod2/mod3 need ModBody (and its per-instance Type
  // combo) exactly like instance 1 — a token match would silently drop them to
  // the generic knob row with no way to reach mod2_type/mod3_type.
  const kind = kindOf(block);
  if (kind === "amp") return <AmpBody />;
  if (kind === "cab") return <CabBody />;
  if (kind === "mod") return <ModBody block={block} />;
  const fxSlot = fxSlotIndexOf(block);
  if (fxSlot !== -1) return <FxSlotBody slot={fxSlot} />;
  return <KnobRow specs={KNOB_SPECS[block]} />;
}
