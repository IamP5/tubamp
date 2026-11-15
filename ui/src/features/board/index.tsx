/**
 * The board — the app's centrepiece (docs/REACT-UI.md §UX structure).
 *
 * A pan/zoom stage carrying the chain on a flat background: In — block tiles —
 * Out, wrapped into rows and joined by quiet wires. Tiles are text tiles (glyph,
 * name, what is loaded in them); they select on click, reorder by 2-D drag
 * (including into new rows), toggle bypass from their power button, and open a
 * More menu for bypass/remove (a remove toasts with Undo). An add tile at the
 * end of the last row opens the sectioned block picker, and every wire gap
 * hides an insert "+" (hover to reveal) that adds at that spot. Look:
 * ui/DESIGN.md.
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
 *  - Rows are `displayRows`: the store's `chainRows` (made region-safe by
 *    `laneRows`) when it has them, and a structure-aware auto-wrap when it does
 *    not — the single source of the partition, so the board and the drag can
 *    never disagree about which row a block is in.
 *  - A split region (docs/SPLIT.md §5) renders as a lane container inside its
 *    row — serial blocks may sit either side of it on the same row: SPLIT, two
 *    half-height lane tracks on the shared column grid, MIX. Each lane is named
 *    in plain text above its first tile (after the split mode: A/B, L/R,
 *    Low/High) and closes with its own small "+" slot, so an empty lane reads
 *    as a slot and adding to a lane is one click. `lane2` is furniture — it
 *    marks where lane B begins and never gets a tile. SPLIT and MIX are not
 *    draggable in v1; their menus (and the panel's trash) offer the three
 *    explicit ways out — keep lane A, keep lane B, run A then B — and the
 *    pickers' "Routing" section creates one (empty, or a one-click dual-amp
 *    rig). A board with no structure lays out exactly as it did before.
 *  - Selection is UI-local (store) and the panel is an overlay over the board's
 *    bottom edge: selecting pans the board so the card is never underneath its
 *    own panel, and a tap on the background dismisses it (R4).
 *  - Any chain change that did not come from a card drag (a picker add, a menu
 *    remove, a preset recall) re-frames the board when the new content no
 *    longer fits the visible band — never mid-gesture.
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
  insertSplit,
  laneSpans,
  sanitizeStructure,
} from "../../chain/structure";
import { useRigCab } from "../../hooks";
import { AMP_SLOTS, CAB_SLOTS, isAmpToken, isCabToken, useStore } from "../../store";
import { BLOCK_INFO } from "../../theme/blocks";
import { stagger, tween } from "../../theme/motion";
import {
  EmptyState,
  FitIcon,
  IconButton,
  MinusIcon,
  PlusIcon,
  Tooltip,
  cx,
} from "../../components";
import type { MenuEntry } from "../../components";
import { BlockCard } from "./BlockCard";
import { Connectors } from "./Connectors";
import { linkWidths, turnText } from "./wires";
import {
  AddBlockButton,
  AddNode,
  InsertNode,
  LaneAddNode,
  LaneLabel,
  OutInfo,
  Terminal,
  TurnPort,
  canAddBlock,
  pickerKinds,
  type SplitOption,
} from "./LaneNodes";
import {
  CARD_H,
  PANEL_BAND,
  REGION_PAD_Y,
  boardGeometry,
  clamp,
  connectorBox,
  contentBox,
  displayRows,
  insertPoints,
  laneAddId,
  laneRows,
  rowsWithRun,
  sameOrder,
  slotOf,
  type LaneRegion,
  type NodeId,
  type Slot,
} from "./layout";
import { laneHint, useLaneNames, useXoverHz, type LaneSide } from "./lanes";
import {
  buildDualAmp,
  canBuildDualAmp,
  removeSplitRegion,
} from "./splitActions";
import { MixCard, SplitCard } from "./StructureCards";
import { useNodeMotion } from "./nodes";
import { useBlockToggles } from "./useBlockToggles";
import { withUndo } from "./undo";
import { useChainDrag } from "./useChainDrag";
import { MAX_ZOOM, MIN_ZOOM, useStageTransform } from "./useStageTransform";
import s from "./board.module.css";

const EMPTY: ReadonlySet<string> = new Set();

/** A file reference as a person reads it: the name, not the path. */
function fileName(path: string): string {
  return path.split(/[\\/]/).pop() || path;
}

/**
 * What is wrong with a plugin slot, if anything: assigned but not installed
 * here, or assigned and not running. Loading and empty are not warnings.
 */
function fxWarning(state: FxSlotState | undefined): string | undefined {
  if (!state?.occupied || state.loading) return undefined;
  if (state.missing)
    return `${state.name} is not installed on this machine. Its settings are preserved.`;
  if (!state.live) return state.error || `${state.name} is not running.`;
  return undefined;
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
  const rigCab = useRigCab();
  const fxSlots = useStore((st) => st.fxSlots);
  const missing = useStore((st) => st.missingMedia);

  /* A tile's warn icon: a capture or IR a session asked for and could not
     get, a second cab stacked on a full-rig capture, a plugin not running. */
  const warningFor = useCallback(
    (id: BlockId): string | undefined => {
      const lost = isAmpToken(id) ? missing[AMP_SLOTS[id].missing] : null;
      if (lost) return `Capture not found: ${fileName(lost)}`;
      if (isCabToken(id)) {
        const lostIr = missing[CAB_SLOTS[id].missing];
        if (lostIr) return `IR not found: ${fileName(lostIr)}`;
        if (id === "cab" && rigCab.doubleCab)
          return "The capture already includes a cab, so this IR stacks a second one.";
      }
      const slot = fxSlotIndexOf(id);
      return slot === -1 ? undefined : fxWarning(fxSlots[slot]);
    },
    [fxSlots, missing, rigCab.doubleCab],
  );

  const removeFromChain = useCallback(
    (id: BlockId) =>
      withUndo(`Removed ${BLOCK_INFO[id].displayName}`, () => removeBlock(id)),
    [removeBlock],
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
  const dualRoom = useMemo(() => canBuildDualAmp(chainOrder), [chainOrder]);
  /* An empty chain shows In — Out and the empty state's one action instead. */
  const showAdd =
    chainOrder.length > 0 && (canAddBlock(kinds) || (!hasStructure && splitRoom));
  const { mode: splitMode, names: laneNames } = useLaneNames();

  /* The published partition, auto-wrapped when there is none, with the split
     region kept whole inside one row. Everything that needs to know where a
     block sits — geometry, drag, connectors — reads THIS, never chainRows. */
  const rows = useMemo(
    () => displayRows(chainOrder, chainRows, showAdd),
    [chainOrder, chainRows, showAdd],
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
    reveal,
  } = useStageTransform({
    content,
    reduced,
    insetBottom: panelInset,
    onBackgroundTap: dismiss,
  });
  const getZoom = useCallback(() => zoom.get(), [zoom]);

  /* A drop is the user placing a card exactly where they want it: the board
     must not re-frame in answer to it. Every other chain change may. */
  const fromDrag = useRef(false);
  const commitDrag = useCallback(
    (order: BlockId[], next: number[]) => {
      fromDrag.current = true;
      setChainOrder(order, next);
    },
    [setChainOrder],
  );

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
    commit: commitDrag,
    reduced,
  });

  const board = useMemo(
    () => boardGeometry(drag.order, drag.rows, showAdd),
    [drag.order, drag.rows, showAdd],
  );
  const slots = board.slots;
  const wireBox = useMemo(() => connectorBox(contentBox(slots)), [slots]);
  const inserts = useMemo(() => insertPoints(board.links), [board.links]);
  const width = useStore((st) => st.width);
  const widths = useMemo(() => linkWidths(width, board.links), [width, board.links]);
  const turns = useMemo(() => {
    if (!width) return [];
    const points: { key: string; x: number; y: number; text: string }[] = [];
    for (const step of width.steps) {
      const text = turnText(step);
      const slot = text ? slotOf(slots, step.block as NodeId) : undefined;
      if (text && slot)
        points.push({ key: step.block, x: slot.x + slot.w, y: slot.y, text });
    }
    return points;
  }, [slots, width]);

  /* Every node springs onto its slot; the dragged/settling card is exempt. */
  useLayoutEffect(() => {
    nodes.layout(slots, drag.dragId ? new Set([drag.dragId]) : EMPTY, reduced);
    nodes.prune(slots);
  }, [drag.dragId, nodes, reduced, slots]);

  /* Opening a panel over the card that summoned it is the one thing an overlay
     panel must not do — pan the minimum that clears it.

     A structural edit made from a menu (a picker add, a split built or taken
     apart, a remove) can also leave the chain hanging off the stage: then the
     whole content is revealed instead, which clears the selected card too —
     the content box contains it. Drops are exempt (`fromDrag`), and nothing
     re-frames while the user is mid-gesture. */
  const framedOrder = useRef(chainOrder);
  const dragging = drag.dragId !== null;

  /* The reveal runs one frame late, through refs: an add publishes the order
     and then selects the new block, and React may render those as two passes —
     the panel inset (and so the visible band) is only right after the second. */
  const latest = useRef({ reveal, content, dragging });
  useLayoutEffect(() => {
    latest.current = { reveal, content, dragging };
  }, [content, dragging, reveal]);

  /* The pending frame lives in a ref and is cancelled only by a newer reveal or
     on unmount — never by the effect's own cleanup. The chain round-trips
     through C++ (`chainChanged` hands back an equal order in a new array), so
     the effect re-runs between scheduling and the frame; a cleanup that
     cancelled would drop the reveal, and the re-run (same order) would not
     schedule another. */
  const revealFrame = useRef(0);
  useEffect(() => () => cancelAnimationFrame(revealFrame.current), []);

  useEffect(() => {
    // Read and cleared before the early return: a drop that only moves a row
    // break leaves the order as it was, and the flag must not survive to make
    // the next menu edit look like a drop.
    const dropped = fromDrag.current;
    fromDrag.current = false;
    if (sameOrder(framedOrder.current, chainOrder)) return;
    framedOrder.current = chainOrder;
    if (dropped) return;
    cancelAnimationFrame(revealFrame.current);
    revealFrame.current = requestAnimationFrame(() => {
      const now = latest.current;
      if (now.dragging || isPanMode()) return;
      now.reveal(now.content);
    });
  }, [chainOrder, isPanMode]);

  /* An emptied chain re-frames, so In — Out sits centred over the empty
     state's line instead of where the old chain's middle was. */
  const empty = chainOrder.length === 0;
  useEffect(() => {
    if (empty) fit(true);
  }, [empty, fit]);

  /* Keyed by value: the chain round-trips through C++ (`chainChanged` hands
     back an equal order in a new array), and re-running on identity alone
     would cancel a reveal that is still animating. */
  const ensured = useRef("");
  useEffect(() => {
    if (selected === null) {
      ensured.current = "";
      return;
    }
    const slot = slotOf(storeSlots, selected);
    if (!slot) return;
    const key = `${selected}@${slot.x},${slot.y}`;
    if (ensured.current === key) return;
    ensured.current = key;
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
      // A middle-button or space-held press is the stage's pan, wherever it
      // starts — ordinary cards let it bubble (useChainDrag), so do these.
      if (event.button !== 0 || isPanMode()) return;
      event.stopPropagation();
      selectBlock(id);
    },
    [isPanMode, selectBlock],
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
      // An auto chain stays auto: `displayRows` re-wraps around the new region
      // on every render, so nothing needs freezing here.
      let rows: number[] = [];
      if (state.chainRows.length > 0) {
        const shown = laneRows(order, state.chainRows);
        const row = at ? at.row : Math.max(shown.length - 1, 0);
        rows = laneRows(next, rowsWithRun(shown, row, SPLIT_TRIPLE.length));
      }

      setChainOrder(next, rows);
      // Same rule as addBlock: what the user just made is what they want to see.
      selectBlock("split");
    },
    [selectBlock, setChainOrder],
  );

  /** The pickers' "Routing" section, bound to an insertion point (the add
   *  tile passes none: append). Absent while a region exists. */
  const splitFor = useCallback(
    (at?: { index: number; row: number }): SplitOption | undefined =>
      hasStructure
        ? undefined
        : {
            enabled: splitRoom,
            onSelect: () => addSplit(at),
            dualAmp: {
              enabled: dualRoom,
              onSelect: () => buildDualAmp(at),
            },
          },
    [addSplit, dualRoom, hasStructure, splitRoom],
  );
  const splitOption = useMemo(() => splitFor(), [splitFor]);

  /** Append to the end of one lane — the lane [+] slot's action. */
  const regionRow = board.region?.row ?? 0;
  const addToLane = useMemo(() => {
    const make =
      (side: LaneSide) =>
      (id: BlockId): void => {
        const order = useStore.getState().chainOrder;
        const spans = laneSpans(findStructure(order));
        if (!spans) return;
        addBlock(id, { index: spans[side].end, row: regionRow });
      };
    return { a: make("a"), b: make("b") };
  }, [addBlock, regionRow]);

  /* "Make dual amp" on the Amp tile, whenever the rig can be built. */
  const ampEntries = useMemo<MenuEntry[] | undefined>(
    () =>
      dualRoom && chainOrder.includes("amp")
        ? [
            {
              id: "make-stereo",
              label: "Make dual amp",
              icon: <StereoGlyph />,
              onSelect: () => buildDualAmp(),
            },
          ]
        : undefined,
    [chainOrder, dualRoom],
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
      onScroll={(event) => {
        // `overflow: hidden` hides the scrollbars, not the scrolling: the browser
        // still scrolls this box to reveal a focused element (Tab onto a card
        // that sits off-stage, any scrollIntoView). The board is placed by the
        // stage transform alone, so a scrolled root shifts every card and
        // everything anchored to the root — and no pan can bring it back.
        const root = event.currentTarget;
        if (root.scrollLeft !== 0 || root.scrollTop !== 0) root.scrollTo(0, 0);
      }}
    >
      <motion.div
        className={cx(s.stage, (panning || drag.dragId) && s.stageActive)}
        style={{ x: panX, y: panY, scale: zoom }}
        data-stage-bg=""
      >
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
                widths={widths}
                region={board.region}
                box={wireBox}
                nodes={nodes}
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
                const common = {
                  id,
                  node: nodes.get(id),
                  selected: selected === id,
                  enabled: toggles[id].value,
                  dragging: drag.dragId === id,
                  reduced,
                  paramIndex: toggles[id].parameterIndex,
                  onPointerDown: structural
                    ? onStructurePointerDown
                    : onCardPointerDown,
                  onToggle: toggles[id].toggle,
                  onRemove: structural
                    ? () => removeSplitRegion("series")
                    : () => removeFromChain(id),
                };
                if (id === "split") return <SplitCard key={id} {...common} />;
                if (id === "mix") return <MixCard key={id} {...common} />;
                return (
                  <BlockCard
                    key={id}
                    {...common}
                    warning={warningFor(id)}
                    extraEntries={id === "amp" ? ampEntries : undefined}
                  />
                );
              })}
            </AnimatePresence>

            {/* Lane furniture: each lane's name above its first tile, and a
                lane-scoped "+" slot closing each lane. */}
            {board.region && (
              <RegionLabels
                region={board.region}
                slots={slots}
                mode={splitMode}
                names={laneNames}
              />
            )}
            <AnimatePresence>
              {board.region &&
                showAdd &&
                (["a", "b"] as const).map((side) => (
                  <LaneAddNode
                    key={laneAddId(side)}
                    side={side}
                    name={laneNames[side]}
                    node={nodes.get(laneAddId(side))}
                    kinds={kinds}
                    reduced={reduced}
                    onAdd={addToLane[side]}
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
                  splitFor={hasStructure ? undefined : splitFor}
                />
              ))}

            {/* Static like the inserts, and hidden while a card is in flight
                for the same reason. */}
            {!drag.dragId &&
              turns.map((turn) => (
                <TurnPort key={`turn-${turn.key}`} x={turn.x} y={turn.y} text={turn.text} />
              ))}

            <Terminal kind="out" node={nodes.get("out")} reduced={reduced}>
              {width?.host.out === 1 && width.wouldBeStereo && <OutInfo />}
            </Terminal>
          </motion.div>
        </div>
      </motion.div>

      {chainOrder.length === 0 && (
        <div className={s.emptyOverlay}>
          <EmptyState
            className={s.empty}
            message="The chain is empty"
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

/* ─────────────────────────────── lane labels ───────────────────────────── */

/**
 * Both lane names, each left-aligned above the first thing in its lane: the
 * first tile, or the lane's add slot while the lane is empty. Lane A's lands
 * in the frame's top pad, lane B's in the gap between the tracks. Its own
 * component so the crossover knob (which the tooltip quotes) re-renders two
 * labels, not the board.
 */
function RegionLabels({
  region,
  slots,
  mode,
  names,
}: {
  region: LaneRegion;
  slots: readonly Slot[];
  mode: number;
  names: Record<LaneSide, string>;
}) {
  const xover = useXoverHz();
  const anchor = (side: LaneSide, track: number) => {
    let first: Slot | undefined;
    for (const slot of slots)
      if (slot.lane === side && (!first || slot.x < first.x)) first = slot;
    // A lane with no tile and no add slot (the chain is full): its track.
    if (!first) return { x: region.laneX, y: track - CARD_H / 2 - LABEL_GAP };
    return { x: first.x, y: first.y - first.h / 2 - LABEL_GAP };
  };
  const a = anchor("a", region.trackA);
  const b = anchor("b", region.trackB);
  return (
    <>
      <LaneLabel side="a" name={names.a} hint={laneHint(mode, "a", xover)} {...a} />
      <LaneLabel side="b" name={names.b} hint={laneHint(mode, "b", xover)} {...b} />
    </>
  );
}

/** Lane label centre line, this far above the element it names. */
const LABEL_GAP = REGION_PAD_Y / 2;

/** Two overlapping rings — the "dual amp" mark for the Amp tile's menu row. */
function StereoGlyph() {
  return (
    <svg width={16} height={16} viewBox="0 0 16 16" aria-hidden="true">
      <circle cx="6" cy="8" r="4" fill="none" stroke="currentColor" strokeWidth={1.5} />
      <circle cx="10" cy="8" r="4" fill="none" stroke="currentColor" strokeWidth={1.5} />
    </svg>
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
 * Bottom-right zoom group: ghost minus / plus, the percentage (click: 100%)
 * and fit. The percentage is written straight into the DOM from the motion
 * value — a zoom gesture must not re-render React.
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
      <Tooltip label="Zoom out">
        <IconButton size="sm" aria-label="Zoom out" onClick={onZoomOut}>
          <MinusIcon />
        </IconButton>
      </Tooltip>
      <Tooltip label="Reset to 100%">
        <button
          ref={label}
          type="button"
          className={s.zoomValue}
          aria-label="Reset zoom to 100%"
          onClick={onReset}
        />
      </Tooltip>
      <Tooltip label="Zoom in">
        <IconButton size="sm" aria-label="Zoom in" onClick={onZoomIn}>
          <PlusIcon />
        </IconButton>
      </Tooltip>
      <Tooltip label="Zoom to fit">
        <IconButton size="sm" aria-label="Zoom to fit" onClick={onFit}>
          <FitIcon />
        </IconButton>
      </Tooltip>
    </motion.div>
  );
}

export { MAX_ZOOM, MIN_ZOOM };
