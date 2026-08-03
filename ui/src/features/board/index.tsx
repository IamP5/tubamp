/**
 * The board — the app's centrepiece (docs/REACT-UI.md §UX structure).
 *
 * A Figma/Miro-style pan/zoom stage carrying one lane: IN — block cards — OUT,
 * joined by connectors with drifting flow chevrons. Cards select on click,
 * reorder by drag, toggle bypass from their LED, and open a kebab menu for
 * bypass/remove. A [+] node at the end of the lane offers every block that is
 * not in the chain yet.
 *
 * Architecture notes worth keeping in mind before editing:
 *
 *  - Positions are analytic, not DOM flow. Every lane element is absolutely
 *    positioned by a motion value (see ./layout.ts + ./nodes.ts). Reflow, drag
 *    and settle are therefore pure transform animations, connectors can be
 *    derived from the same numbers, and no Motion `layout` projection runs
 *    inside the scaled stage (projection would double-apply the stage scale).
 *  - Chain order is state, never a parameter. The only write to the plugin is
 *    the store action on drop / add / remove; `chainChanged` coming back in is
 *    applied to the store and compared by value here, so an inbound event can
 *    never bounce back out as another `setChainOrder`.
 *  - Selection is UI-local (store), and adding a block force-selects it while
 *    removal falls back amp → first → none — both live in the store actions.
 */
import {
  useCallback,
  useLayoutEffect,
  useMemo,
  useRef,
  type PointerEvent as ReactPointerEvent,
} from "react";
import {
  AnimatePresence,
  animate,
  motion,
  useMotionValue,
  useMotionValueEvent,
  useReducedMotion,
  type MotionValue,
} from "motion/react";
import type { BlockId } from "../../bridge/types";
import { useRigCab } from "../../hooks";
import { useStore } from "../../store";
import { spring, stagger, tween } from "../../theme/motion";
import { EmptyState, PlusIcon, cx } from "../../components";
import { BlockCard, type CardNote } from "./BlockCard";
import { Connectors } from "./Connectors";
import {
  AddBlockButton,
  AddNode,
  Terminal,
  absentBlocks,
} from "./LaneNodes";
import { CARD_H, DOCK_INSET, computeSlots, totalWidth } from "./layout";
import { useNodeMotion } from "./nodes";
import { useBlockToggles, bypassedBlocks } from "./useBlockToggles";
import { useChainDrag } from "./useChainDrag";
import { MAX_ZOOM, MIN_ZOOM, useStageTransform } from "./useStageTransform";
import s from "./board.module.css";

const INSTANT = { duration: 0 } as const;
const EMPTY: ReadonlySet<string> = new Set();

/* Full-rig captures (see useRigCab): the amp states it, the cab warns about it. */
const AMP_NOTE: CardNote = {
  label: "+ CAB",
  tip: "This capture was taken through a cabinet — it already includes the speaker.",
  tone: "info",
};
const CAB_NOTE: CardNote = {
  label: "2× CAB",
  tip: "The loaded capture already includes a cab, so this IR stacks a second one.",
  tone: "warn",
};

/** Staggered board build-in (motion-design.md §2.1), gated on reduced motion. */
const LANE_VARIANTS = {
  hidden: {},
  show: { transition: stagger.board },
};
const LANE_VARIANTS_REDUCED = {
  hidden: {},
  show: { transition: { staggerChildren: 0 } },
};

export function Board() {
  const chainOrder = useStore((st) => st.chainOrder);
  const selected = useStore((st) => st.selected);
  const selectBlock = useStore((st) => st.selectBlock);
  const addBlock = useStore((st) => st.addBlock);
  const removeBlock = useStore((st) => st.removeBlock);
  const setChainOrder = useStore((st) => st.setChainOrder);

  const reduced = useReducedMotion() ?? false;
  const toggles = useBlockToggles();
  const bypassed = useMemo(() => bypassedBlocks(toggles), [toggles]);
  const rigCab = useRigCab();

  const noteFor = useCallback(
    (id: BlockId): CardNote | undefined => {
      if (id === "amp") return rigCab.captureHasCab ? AMP_NOTE : undefined;
      if (id === "cab") return rigCab.doubleCab ? CAB_NOTE : undefined;
      return undefined;
    },
    [rigCab.captureHasCab, rigCab.doubleCab],
  );

  const nodes = useNodeMotion();
  const laneRef = useRef<HTMLDivElement | null>(null);
  const laneX = useMotionValue(0);
  const lanePlaced = useRef(false);

  const absent = useMemo(() => absentBlocks(chainOrder), [chainOrder]);
  const showAdd = absent.length > 0;

  // Lane width only depends on how MANY cards there are, so it can be measured
  // from the store order — before the drag hook, which needs the stage.
  const total = useMemo(
    () => totalWidth(computeSlots(chainOrder, showAdd)),
    [chainOrder, showAdd],
  );
  const content = useMemo(() => ({ w: total, h: CARD_H + 56 }), [total]);

  const {
    containerRef: stageRef,
    panX,
    panY,
    zoom,
    isPanMode,
    panMode,
    panning,
    onPointerDown: onStagePointerDown,
    onDoubleClick: onStageDoubleClick,
    zoomBy,
    zoomTo,
    fit,
  } = useStageTransform(content, reduced, DOCK_INSET);
  const getZoom = useCallback(() => zoom.get(), [zoom]);

  const drag = useChainDrag({
    chainOrder,
    showAdd,
    nodes,
    laneRef,
    getZoom,
    isPanMode,
    onSelect: selectBlock,
    commit: setChainOrder,
    reduced,
  });

  const slots = useMemo(
    () => computeSlots(drag.order, showAdd),
    [drag.order, showAdd],
  );

  /* Lane stays centred on the stage origin as it grows and shrinks. */
  useLayoutEffect(() => {
    const target = -total / 2;
    if (!lanePlaced.current) {
      lanePlaced.current = true;
      laneX.set(target);
      return;
    }
    if (laneX.get() !== target) {
      animate(laneX, target, reduced ? INSTANT : spring.reflow);
    }
  }, [laneX, reduced, total]);

  /* Every node springs onto its slot; the dragged/settling card is exempt. */
  useLayoutEffect(() => {
    nodes.layout(slots, drag.dragId ? new Set([drag.dragId]) : EMPTY, reduced);
    nodes.prune(slots);
  }, [drag.dragId, nodes, reduced, slots]);

  /*
   * The native context menu is suppressed app-wide in App.tsx (it must hold
   * even when the board is unmounted). The local handler below is belt-and-
   * braces for the stage itself.
   */

  const onCardPointerDown = useCallback(
    (event: ReactPointerEvent<HTMLElement>, id: BlockId) =>
      drag.beginDrag(event, id),
    [drag],
  );

  const laneVariants = reduced ? LANE_VARIANTS_REDUCED : LANE_VARIANTS;

  return (
    <div
      ref={stageRef}
      className={s.root}
      data-pan={panning ? "active" : panMode ? "ready" : undefined}
      onPointerDown={onStagePointerDown}
      onDoubleClick={onStageDoubleClick}
      onContextMenu={(event) => event.preventDefault()}
    >
      <motion.div
        className={cx(s.stage, (panning || drag.dragId) && s.stageActive)}
        style={{ x: panX, y: panY, scale: zoom }}
        data-stage-bg=""
      >
        <div className={s.grid} data-stage-bg="" />

        <div className={s.lane}>
          <motion.div
            ref={laneRef}
            className={s.laneInner}
            style={{ x: laneX }}
            variants={laneVariants}
            initial="hidden"
            animate="show"
          >
            <div className={s.wireLayer}>
              <Connectors
                slots={slots}
                nodes={nodes}
                selected={selected}
                bypassed={bypassed}
                reduced={reduced}
              />
            </div>

            <Terminal kind="in" node={nodes.get("in")} reduced={reduced} />

            <AnimatePresence>
              {drag.order.map((id) => (
                <BlockCard
                  key={id}
                  id={id}
                  node={nodes.get(id)}
                  selected={selected === id}
                  enabled={toggles[id].value}
                  dragging={drag.dragId === id}
                  reduced={reduced}
                  note={noteFor(id)}
                  paramIndex={toggles[id].parameterIndex}
                  onPointerDown={onCardPointerDown}
                  onToggle={toggles[id].toggle}
                  onRemove={() => removeBlock(id)}
                />
              ))}
            </AnimatePresence>

            <AnimatePresence>
              {showAdd && (
                <AddNode
                  key="add"
                  node={nodes.get("add")}
                  absent={absent}
                  reduced={reduced}
                  onAdd={addBlock}
                />
              )}
            </AnimatePresence>

            <Terminal kind="out" node={nodes.get("out")} reduced={reduced} />
          </motion.div>
        </div>
      </motion.div>

      {chainOrder.length === 0 && (
        <div className={s.emptyOverlay}>
          <EmptyState
            className={s.empty}
            message="Empty chain"
            hint="Signal passes straight from IN to OUT."
            action={<AddBlockButton absent={absent} onAdd={addBlock} />}
          />
        </div>
      )}

      <ZoomControls
        zoom={zoom}
        onZoomIn={() => zoomBy(1.25)}
        onZoomOut={() => zoomBy(1 / 1.25)}
        onReset={() => zoomTo(1)}
        onFit={() => fit(true)}
      />
    </div>
  );
}

/* ─────────────────────────────── zoom control ──────────────────────────── */

interface ZoomControlsProps {
  zoom: MotionValue<number>;
  onZoomIn(): void;
  onZoomOut(): void;
  onReset(): void;
  onFit(): void;
}

/**
 * Bottom-right zoom read-out. The percentage is written straight into the DOM
 * from the motion value — a zoom gesture must not re-render React.
 */
function ZoomControls({
  zoom,
  onZoomIn,
  onZoomOut,
  onReset,
  onFit,
}: ZoomControlsProps) {
  const label = useRef<HTMLButtonElement | null>(null);
  const write = useCallback((value: number) => {
    if (label.current) label.current.textContent = `${Math.round(value * 100)}%`;
  }, []);

  useMotionValueEvent(zoom, "change", write);
  useLayoutEffect(() => write(zoom.get()), [write, zoom]);

  return (
    <motion.div
      className={s.zoomBar}
      initial={{ opacity: 0 }}
      animate={{ opacity: 1 }}
      transition={{ ...tween.enter, delay: 0.24 }}
      onPointerDown={(event) => event.stopPropagation()}
      onDoubleClick={(event) => event.stopPropagation()}
    >
      <button
        type="button"
        className={s.zoomButton}
        aria-label="Zoom out"
        onClick={onZoomOut}
      >
        <svg width="14" height="14" viewBox="0 0 16 16" aria-hidden="true">
          <path
            d="M3.5 8h9"
            stroke="currentColor"
            strokeWidth={1.5}
            strokeLinecap="round"
          />
        </svg>
      </button>
      <button
        ref={label}
        type="button"
        className={s.zoomValue}
        aria-label="Reset zoom to 100%"
        onClick={onReset}
      />
      <button
        type="button"
        className={s.zoomButton}
        aria-label="Zoom in"
        onClick={onZoomIn}
      >
        <PlusIcon size={14} />
      </button>
      <button
        type="button"
        className={cx(s.zoomButton, s.zoomFit)}
        aria-label="Zoom to fit"
        onClick={onFit}
      >
        FIT
      </button>
    </motion.div>
  );
}

export { MAX_ZOOM, MIN_ZOOM };
