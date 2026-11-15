/**
 * The block editor: the selected block's controls, in a flush band on the
 * stage's bottom edge (ui/DESIGN.md §Layout, docs/REACT-UI.md §UX structure).
 *
 * Structure: a 44px header — power (the block's `*_on` param), the block's
 * name, a slot the block's own editor fills with its type/mode controls,
 * previous/next block, remove, close — over a body that switches per block
 * kind. Selection is UI-local store state; with nothing selected the band is
 * not rendered at all and the board has the whole stage.
 *
 * Every editor follows one grammar (./ui.tsx): a display that draws what the
 * effect does, then the knobs, most important first. `data-narrow` is set on
 * the band when it is too narrow for a display (the TONE3000 drawer open in a
 * small window), and editors drop the display rather than squeeze the knobs.
 *
 * Motion: the band slides in once; switching blocks crossfades the body in
 * place (opacity only), so knob positions stay visually stable.
 */
import { useCallback, useMemo, useState } from "react";
import { AnimatePresence, motion, useReducedMotion } from "motion/react";
import {
  ChevronLeftIcon,
  ChevronRightIcon,
  CloseIcon,
  IconButton,
  PowerPill,
  Tooltip,
  TrashIcon,
} from "../../components";
import { useToggleParam } from "../../hooks";
import { isAmpToken, isCabToken, useStore } from "../../store";
import { BLOCK_INFO } from "../../theme/blocks";
import { powerTip } from "../board/lanes";
import { duration, ease } from "../../theme/motion";
import {
  fxSlotIndexOf,
  isStructureBlockId,
  kindOf,
  type BlockId,
} from "../../bridge";
import { AmpBody } from "./AmpBody";
import { CabBody } from "./CabBody";
import { CompBody } from "./CompBody";
import { DelayBody } from "./DelayBody";
import { DriveBody } from "./DriveBody";
import { EqBody } from "./EqBody";
import { FxSlotBody } from "./FxSlotBody";
import { GateBody } from "./GateBody";
import { MixBody } from "./MixBody";
import { ReverbBody } from "./ReverbBody";
import { ModBody } from "./ModBody";
import { SplitBody } from "./SplitBody";
import { SplitRemoveButton } from "./SplitRemoveButton";
import { ToneModal } from "./ToneModal";
import { HeaderSlotContext, NarrowContext, useNarrow } from "./ui";
import s from "./frame.module.css";

export function Panel() {
  const selected = useStore((st) => st.selected);
  const reduced = useReducedMotion() ?? false;
  const [slot, setSlot] = useState<HTMLDivElement | null>(null);
  const [dock, setDock] = useState<HTMLDivElement | null>(null);
  const narrow = useNarrow(dock);

  return (
    <>
      <AnimatePresence initial={false}>
        {selected !== null && (
          <motion.div
            key="dock"
            ref={setDock}
            className={s.dock}
            data-block={selected}
            data-family={BLOCK_INFO[selected].family}
            data-narrow={narrow || undefined}
            initial={reduced ? { opacity: 0 } : { opacity: 0, y: 8 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{
              opacity: 0,
              y: reduced ? 0 : 8,
              transition: { duration: duration.fast, ease: ease.in },
            }}
            transition={{
              duration: reduced ? duration.fast : duration.base,
              ease: ease.out,
            }}
          >
            <PanelHeader block={selected} slotRef={setSlot} />

            <NarrowContext.Provider value={narrow}>
              <HeaderSlotContext.Provider value={slot}>
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
              </HeaderSlotContext.Provider>
            </NarrowContext.Provider>
          </motion.div>
        )}
      </AnimatePresence>

      {/* Lives outside the band: a TONE3000 download must not depend on which
          block (if any) is selected. */}
      <ToneModal />
    </>
  );
}

/* ─────────────────────────────── header ────────────────────────────────── */

function PanelHeader({
  block,
  slotRef,
}: {
  block: BlockId;
  slotRef: (el: HTMLDivElement | null) => void;
}) {
  const info = BLOCK_INFO[block];
  const power = useToggleParam(info.enableParamId);
  const removeBlock = useStore((st) => st.removeBlock);
  const selectBlock = useStore((st) => st.selectBlock);
  const chainOrder = useStore((st) => st.chainOrder);
  // Previous / next walk the chain in signal order; `lane2` is furniture and
  // never selectable.
  const stops = useMemo<BlockId[]>(
    () => chainOrder.filter((id) => id !== "lane2"),
    [chainOrder],
  );
  const at = stops.indexOf(block);
  const step = useCallback(
    (delta: number) => {
      const next = stops[at + delta];
      if (next) selectBlock(next);
    },
    [at, selectBlock, stops],
  );

  const removeLabel = isStructureBlockId(block)
    ? "Remove split (lanes rejoin)"
    : "Remove this block from the chain";

  return (
    <header className={s.header}>
      <span data-param-index={power.parameterIndex}>
        <Tooltip label={powerTip(block)}>
          <PowerPill on={power.value} onChange={power.setValue} />
        </Tooltip>
      </span>

      {/* Always the block's own name. A plugin slot stays "Plugin 1": the
          hosted plugin's name is in the slot's picker, beside it. */}
      <span className={s.title}>{info.displayName}</span>

      <div className={s.slot} ref={slotRef} />

      <div className={s.actions}>
        <span className={s.stepBlock}>
          <Tooltip label="Previous block">
            <IconButton
              aria-label="Previous block"
              disabled={at <= 0}
              onClick={() => step(-1)}
            >
              <ChevronLeftIcon />
            </IconButton>
          </Tooltip>
          <Tooltip label="Next block">
            <IconButton
              aria-label="Next block"
              disabled={at === -1 || at >= stops.length - 1}
              onClick={() => step(1)}
            >
              <ChevronRightIcon />
            </IconButton>
          </Tooltip>
          <span className={s.actionsGap} />
        </span>
        {/* Split and Mixer have editors, so remove can reach them — and removing
            either removes the whole region, which has three ways out (keep lane
            A, keep lane B, chain A into B): their button opens the same menu the
            board's structural tiles show (features/board/splitActions). */}
        {isStructureBlockId(block) ? (
          <SplitRemoveButton />
        ) : (
          <Tooltip label={removeLabel}>
            <IconButton
              aria-label={removeLabel}
              destructive
              onClick={() => removeBlock(block)}
            >
              <TrashIcon />
            </IconButton>
          </Tooltip>
        )}
        {/* Explicit dismiss: a tap on the board's background closes the editor
            too, but it must not be the only way (panning shares that gesture). */}
        <Tooltip label="Close">
          <IconButton
            aria-label="Close panel"
            onClick={() => selectBlock(null)}
          >
            <CloseIcon />
          </IconButton>
        </Tooltip>
      </div>
    </header>
  );
}

/* ──────────────────────────── body dispatch ────────────────────────────── */

function BlockBody({ block }: { block: BlockId }) {
  // Keyed by KIND, not token: mod2/mod3 need ModBody (and its per-instance type
  // combo) exactly like instance 1. Same for every other duplicable kind.
  const kind = kindOf(block);
  if (isAmpToken(block)) return <AmpBody block={block} />;
  if (isCabToken(block)) return <CabBody block={block} />;
  if (kind === "gate") return <GateBody block={block} />;
  if (kind === "comp") return <CompBody block={block} />;
  if (kind === "drive") return <DriveBody block={block} />;
  if (kind === "eq") return <EqBody block={block} />;
  if (kind === "mod") return <ModBody block={block} />;
  if (kind === "delay") return <DelayBody block={block} />;
  if (kind === "reverb") return <ReverbBody block={block} />;
  if (kind === "split") return <SplitBody />;
  if (kind === "mix") return <MixBody />;
  // `lane2` is furniture (docs/SPLIT.md §1): the board never renders it as a
  // tile, so selection should never reach it; a null body is the backstop.
  if (kind === "lane2") return null;
  const fxSlot = fxSlotIndexOf(block);
  return fxSlot === -1 ? null : <FxSlotBody slot={fxSlot} />;
}
