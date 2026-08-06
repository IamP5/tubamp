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
 *    the drag can never disagree about which row a block is in), then through
 *    `laneRows`, which regroups a split region into the single row-equivalent
 *    the container is.
 *  - A split region (docs/SPLIT.md §5) renders as a lane container: SPLIT, two
 *    half-height lane tracks on the shared column grid, MIX. `lane2` is
 *    furniture — it marks where lane B begins and never gets a card, so lane
 *    membership is nothing but position in the flat order and needs no
 *    recomputing at commit. SPLIT and MIX are not draggable in v1; removing
 *    either flattens the region (lane A then lane B, inline where the split
 *    was), and "Add split" in the [+] picker creates one. A board with no
 *    structure lays out exactly as it did before any of this existed.
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
  isStructureBlockId,
  type BlockId,
  type FxSlotState,
} from "../../bridge/types";
import {
  SPLIT_TRIPLE,
  canAddSplit,
  findStructure,
  flattenStructure,
  insertSplit,
  sanitizeStructure,
} from "../../chain/structure";
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
  boardGeometry,
  clamp,
  connectorBox,
  contentBox,
  insertPoints,
  laneRows,
  rowContaining,
  rowsWithRun,
  rowsWithoutRun,
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
  /* Exactly one split region per chain: once the order carries any structural
     token the "Add split" row is gone, not greyed. `canAddSplit` also holds the
     length cap — the fork costs three tokens. */
  const hasStructure = useMemo(() => {
    const { splitAt, lane2At, mixAt } = findStructure(chainOrder);
    return splitAt >= 0 || lane2At >= 0 || mixAt >= 0;
  }, [chainOrder]);
  const splitRoom = useMemo(() => canAddSplit(chainOrder), [chainOrder]);
  const showAdd = canAddBlock(kinds) || (!hasStructure && splitRoom);

  /* The published partition, auto-wrapped when there is none and regrouped so a
     split region owns one row. Everything that needs to know where a block sits
     — geometry, drag, connectors — reads THIS, never chainRows directly. */
  const rows = useMemo(
    () => laneRows(chainOrder, rowsFor(chainOrder, chainRows)),
    [chainOrder, chainRows],
  );

  /* Framing geometry comes from the STORE order, not the drag preview: the
     stage must not re-fit or re-leash itself while a card is in flight. It is
     also needed before the drag hook, which needs the stage. */
  const storeSlots = useMemo(
    () => boardGeometry(chainOrder, rows, showAdd).slots,
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

  const board = useMemo(
    () => boardGeometry(drag.order, drag.rows, showAdd),
    [drag.order, drag.rows, showAdd],
  );
  const slots = board.slots;
  const wireBox = useMemo(() => connectorBox(contentBox(slots)), [slots]);
  const inserts = useMemo(() => insertPoints(board.links), [board.links]);

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

  /* SPLIT / MIX are not draggable in v1 (docs/SPLIT.md §5 offers this as the
     simpler of the two options): moving the pair around would have to keep it a
     pair, and a half-moved pair is a chain the sanitizer flattens under the
     user's hands. With no pick-up to keep the panel clear of, the press IS the
     selection; stopPropagation keeps it off the stage's pan / background tap. */
  const onStructurePointerDown = useCallback(
    (event: ReactPointerEvent<HTMLElement>, id: BlockId) => {
      event.stopPropagation();
      selectBlock(id);
    },
    [selectBlock],
  );

  /**
   * Insert an empty fork — `split, lane2, mix` — at a gap, or at the end from
   * the tail [+]. One write, like every other chain edit: the three tokens are
   * one shape, and publishing them in stages would put a lone `split` in front
   * of the audio thread, which the C++ sanitizer would flatten straight back.
   */
  const addSplit = useCallback(
    (at?: { index: number; row: number }) => {
      const state = useStore.getState();
      const order = state.chainOrder;
      if (!canAddSplit(order)) return;

      const index = at ? clamp(at.index, 0, order.length) : order.length;
      const next = sanitizeStructure(insertSplit(order, index));
      const displayed = laneRows(order, rowsFor(order, state.chainRows));
      const row = at ? at.row : Math.max(displayed.length - 1, 0);
      // An auto chain stays auto: `laneRows` regroups the wrap around the new
      // container on every render, so nothing needs freezing here.
      const rows =
        state.chainRows.length === 0
          ? []
          : laneRows(next, rowsWithRun(displayed, row, SPLIT_TRIPLE.length));

      setChainOrder(next, rows);
      // Same rule as addBlock: what the user just made is what they want to see.
      selectBlock("split");
    },
    [selectBlock, setChainOrder],
  );

  /**
   * Deleting SPLIT or MIX deletes the region: lane A's blocks then lane B's,
   * inline where the split stood — which is exactly the flat order with the
   * three structural tokens dropped out of it.
   */
  const flatten = useCallback(() => {
    const state = useStore.getState();
    const order = state.chainOrder;
    const structure = findStructure(order);
    if (structure.splitAt < 0 && structure.lane2At < 0 && structure.mixAt < 0)
      return;

    const next = flattenStructure(order);
    const displayed = laneRows(order, rowsFor(order, state.chainRows));
    const rows =
      state.chainRows.length === 0 || !structure.valid
        ? []
        : rowsWithoutRun(
            displayed,
            rowContaining(displayed, structure.splitAt),
            order.length - next.length,
          );

    setChainOrder(next, rows);
  }, [setChainOrder]);

  /* The picker's structural row, bound once: absent while a region exists,
     disabled when the chain has no room for one. */
  const splitOption = useMemo(
    () =>
      hasStructure
        ? undefined
        : { enabled: splitRoom, onSelect: () => addSplit() },
    [addSplit, hasStructure, splitRoom],
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
                links={board.links}
                region={board.region}
                box={wireBox}
                nodes={nodes}
                selected={selected}
                bypassed={bypassed}
                reduced={reduced}
              />
            </div>

            <Terminal kind="in" node={nodes.get("in")} reduced={reduced} />

            <AnimatePresence>
              {drag.order.map((id) => {
                // `lane2` marks where lane B starts; it is never a card, never a
                // drop target and never has a panel.
                if (id === "lane2") return null;
                const structural = isStructureBlockId(id);
                return (
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
                    onPointerDown={
                      structural ? onStructurePointerDown : onCardPointerDown
                    }
                    onToggle={toggles[id].toggle}
                    onRemove={structural ? flatten : () => removeBlock(id)}
                    removeLabel={
                      structural ? "Remove split (lanes rejoin)" : undefined
                    }
                  />
                );
              })}
            </AnimatePresence>

            <AnimatePresence>
              {showAdd && (
                <AddNode
                  key="add"
                  node={nodes.get("add")}
                  kinds={kinds}
                  reduced={reduced}
                  onAdd={addBlock}
                  onAddSplit={splitOption}
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
                  onAddSplit={hasStructure ? undefined : addSplit}
                  canAddSplit={splitRoom}
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
            action={
              <AddBlockButton
                kinds={kinds}
                onAdd={addBlock}
                onAddSplit={splitOption}
              />
            }
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
