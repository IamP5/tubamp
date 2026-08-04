/**
 * The board — the app's centrepiece (docs/REACT-UI.md §UX structure).
 *
 * A Figma/Miro-style pan/zoom stage carrying the chain: IN — block cards — OUT,
 * wrapped into rows and joined by connectors with drifting flow chevrons. Cards
 * select on click, reorder by 2-D drag (including into new rows), toggle bypass
 * from their LED, and open a kebab menu for bypass/remove. A [+] node at the end
 * of the last row offers every kind that still has a free instance, and every
 * connector gap hides an insert [+] (hover to reveal) that adds at that spot.
 *
 * Architecture notes worth keeping in mind before editing:
 *
 *  - Positions are analytic, not DOM flow. Every element is absolutely
 *    positioned by a pair of motion values (see ./layout.ts + ./nodes.ts).
 *    Reflow, drag and settle are therefore pure transform animations, connectors
 *    can be derived from the same numbers, and no Motion `layout` projection
 *    runs inside the scaled stage (projection would double-apply the scale).
 *  - Chain order is state, never a parameter. The only write to the plugin is
 *    the store action on drop / add / remove; `chainChanged` coming back in is
 *    applied to the store and compared by value here, so an inbound event can
 *    never bounce back out as another `setChainOrder`.
 *  - Rows are the store's `chainRows` when it has them and an auto-wrap when it
 *    does not (`rowsFor` — the single source of the partition, so the board and
 *    the drag can never disagree about which row a block is in).
 *  - Selection is UI-local (store) and the panel is an overlay over the board's
 *    bottom edge: selecting pans the board so the card is never underneath its
 *    own panel, and a tap on the background dismisses it (R4).
 */
import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  type PointerEvent as ReactPointerEvent,
} from "react";
import {
  AnimatePresence,
  motion,
  useMotionValueEvent,
  useReducedMotion,
  type MotionValue,
} from "motion/react";
import {
  fxSlotIndexOf,
  type BlockId,
  type FxSlotState,
} from "../../bridge/types";
import { useRigCab } from "../../hooks";
import { rowsFor, useStore } from "../../store";
import { stagger, tween } from "../../theme/motion";
import { EmptyState, PlusIcon, cx } from "../../components";
import { BlockCard, type CardNote } from "./BlockCard";
import { Connectors } from "./Connectors";
import {
  AddBlockButton,
  AddNode,
  InsertNode,
  Terminal,
  canAddBlock,
  pickerKinds,
} from "./LaneNodes";
import {
  PANEL_BAND,
  computeSlots,
  connectorBox,
  contentBox,
  insertPoints,
  slotOf,
} from "./layout";
import { useNodeMotion } from "./nodes";
import { useBlockToggles, bypassedBlocks } from "./useBlockToggles";
import { useChainDrag } from "./useChainDrag";
import { MAX_ZOOM, MIN_ZOOM, useStageTransform } from "./useStageTransform";
import s from "./board.module.css";

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

/**
 * An fx card keeps its slot caption ("FX 1") and carries the hosted plugin as a
 * note, the same channel the amp/cab pair uses for secondary information — the
 * card is the slot, the note is what is currently in it.
 */
function fxNote(state: FxSlotState | undefined): CardNote | undefined {
  if (!state) return undefined;
  if (!state.occupied)
    return {
      label: "EMPTY",
      tip: "No plugin loaded — this slot passes audio through untouched.",
      tone: "info",
    };
  if (state.loading)
    return { label: "LOADING", tip: `Loading ${state.name}…`, tone: "info" };
  if (state.missing)
    return {
      label: state.name,
      tip: `${state.name} is not installed on this machine. Its settings are preserved.`,
      tone: "warn",
    };
  if (!state.live)
    return {
      label: state.name,
      tip: state.error || `${state.name} is not running.`,
      tone: "warn",
    };
  return {
    label: state.name,
    tip: `${state.name} — ${state.manufacturer}`,
    tone: "info",
  };
}

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
  const chainRows = useStore((st) => st.chainRows);
  const selected = useStore((st) => st.selected);
  const selectBlock = useStore((st) => st.selectBlock);
  const addBlock = useStore((st) => st.addBlock);
  const removeBlock = useStore((st) => st.removeBlock);
  const setChainOrder = useStore((st) => st.setChainOrder);

  const reduced = useReducedMotion() ?? false;
  const toggles = useBlockToggles();
  const bypassed = useMemo(() => bypassedBlocks(toggles), [toggles]);
  const rigCab = useRigCab();
  const fxSlots = useStore((st) => st.fxSlots);

  const noteFor = useCallback(
    (id: BlockId): CardNote | undefined => {
      if (id === "amp") return rigCab.captureHasCab ? AMP_NOTE : undefined;
      if (id === "cab") return rigCab.doubleCab ? CAB_NOTE : undefined;
      const slot = fxSlotIndexOf(id);
      return slot < 0 ? undefined : fxNote(fxSlots[slot]);
    },
    [fxSlots, rigCab.captureHasCab, rigCab.doubleCab],
  );

  const nodes = useNodeMotion();
  const laneRef = useRef<HTMLDivElement | null>(null);

  const kinds = useMemo(() => pickerKinds(chainOrder), [chainOrder]);
  const showAdd = canAddBlock(kinds);

  /* The published partition, auto-wrapped when there is none. Everything that
     needs to know where a block sits — geometry, drag, connectors — reads THIS,
     never chainRows directly. */
  const rows = useMemo(
    () => rowsFor(chainOrder, chainRows),
    [chainOrder, chainRows],
  );

  /* Framing geometry comes from the STORE order, not the drag preview: the
     stage must not re-fit or re-leash itself while a card is in flight. It is
     also needed before the drag hook, which needs the stage. */
  const storeSlots = useMemo(
    () => computeSlots(chainOrder, rows, showAdd),
    [chainOrder, rows, showAdd],
  );
  const content = useMemo(() => contentBox(storeSlots), [storeSlots]);

  /* The panel is an overlay: it covers the bottom band of the board exactly
     while a block is selected, and nothing at all otherwise. */
  const panelInset = selected === null ? 0 : PANEL_BAND;
  const dismiss = useCallback(() => selectBlock(null), [selectBlock]);

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
    ensureVisible,
  } = useStageTransform({
    content,
    reduced,
    insetBottom: panelInset,
    onBackgroundTap: dismiss,
  });
  const getZoom = useCallback(() => zoom.get(), [zoom]);

  const drag = useChainDrag({
    chainOrder,
    chainRows: rows,
    showAdd,
    nodes,
    laneRef,
    stageRef,
    getZoom,
    isPanMode,
    onSelect: selectBlock,
    commit: setChainOrder,
    reduced,
  });

  const slots = useMemo(
    () => computeSlots(drag.order, drag.rows, showAdd),
    [drag.order, drag.rows, showAdd],
  );
  const wireBox = useMemo(() => connectorBox(contentBox(slots)), [slots]);
  const inserts = useMemo(() => insertPoints(slots), [slots]);

  /* Every node springs onto its slot; the dragged/settling card is exempt. */
  useLayoutEffect(() => {
    nodes.layout(slots, drag.dragId ? new Set([drag.dragId]) : EMPTY, reduced);
    nodes.prune(slots);
  }, [drag.dragId, nodes, reduced, slots]);

  /* Opening a panel over the card that summoned it is the one thing an overlay
     panel must not do — pan the minimum that clears it. */
  useEffect(() => {
    if (selected === null) return;
    const slot = slotOf(storeSlots, selected);
    if (!slot) return;
    ensureVisible({
      x: slot.x,
      y: slot.y - slot.h / 2,
      w: slot.w,
      h: slot.h,
    });
  }, [ensureVisible, selected, storeSlots]);

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

        {/* The board's origin is the stage origin: with rows there is nothing
            to centre on it, and framing is the stage transform's job (fit /
            ensure-visible), so the content keeps still while it is edited. */}
        <div className={s.lane}>
          <motion.div
            ref={laneRef}
            className={s.laneInner}
            variants={laneVariants}
            initial="hidden"
            animate="show"
          >
            <div className={s.wireLayer}>
              <Connectors
                slots={slots}
                box={wireBox}
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
                  kinds={kinds}
                  reduced={reduced}
                  onAdd={addBlock}
                />
              )}
            </AnimatePresence>

            {/* Hover-revealed inserts, one per gap. Positioned straight from
                the slots (no motion values): they are invisible except under
                the pointer, and they hide while a card is in flight — the two
                states in which a static position could be seen to lag. */}
            {showAdd &&
              !drag.dragId &&
              inserts.map((point) => (
                <InsertNode
                  key={`insert-${point.index}`}
                  point={point}
                  kinds={kinds}
                  onAdd={addBlock}
                />
              ))}

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
            action={<AddBlockButton kinds={kinds} onAdd={addBlock} />}
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
