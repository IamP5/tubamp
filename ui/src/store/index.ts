/**
 * The app store: a zustand store hydrated once from `getUiState()` and kept in
 * sync by every bridge event. Parameter values do NOT live here — they live in
 * the JUCE relays and are read through the param hooks (src/hooks). Meter
 * levels do not live here either (motion values, see useMeters).
 *
 * Selection is UI-local state: it is never sent to C++ and never persisted.
 */
import { create } from "zustand";
import {
  bridge,
  isBlockId,
  isFxBlockId,
  isStructureBlockId,
  fxSlotIndexOf,
  instanceTokensOfKind,
  kindOf,
  BLOCK_KINDS,
  FX_BLOCK_IDS,
  type AbSlot,
  type AbState,
  type BaseBlockId,
  type BlockId,
  type EditorSizeLimits,
  type FileEntry,
  type FxEmbedState,
  type FxSlotIndex,
  type FxSlotState,
  type IrInfo,
  type MissingMedia,
  type ModelInfo,
  type PresetInfo,
  type T3kBrowseKind,
  type T3kBrowseRequest,
  type T3kBrowseShelf,
  type T3kBrowseSort,
  type T3kInstalledFile,
  type T3kModel,
  type T3kState,
  type T3kTarget,
  type T3kTone,
  type UiState,
} from "../bridge";
import {
  MAX_CHAIN_LENGTH,
  flattenStructure,
  sanitizeStructure,
} from "../chain/structure";

export type ToastKind = "info" | "success" | "warning" | "error";

export interface Toast {
  id: number;
  kind: ToastKind;
  message: string;
  /** ms before auto-dismiss; 0 = sticky. */
  duration: number;
}

export interface T3kToneResult {
  toneId: number;
  models: T3kModel[];
}

/** What a download in flight is for — the completion event only carries a path. */
export interface T3kPendingDownload {
  name: string;
  kind: T3kModel["kind"];
  /** null for the select flow, which has no browser row to credit. */
  toneId: number | null;
  /** The block a browser load is for; absent/null = route by `kind` (select
   *  flow, legacy callers). */
  target?: T3kTarget | null;
}

/**
 * The TONE3000 drawer, remembered for the editor's lifetime (closing it only
 * flips `open`): the request it shows, the detail level, and through
 * `t3kLists`/`t3kScroll` the pages and scroll offset of every list it showed.
 */
export interface T3kBrowserState {
  open: boolean;
  kind: T3kBrowseKind;
  /** Which NAM engine captures load into; IRs always go to the cab. Read it
   *  through `selectT3kTarget`, which also falls back to `amp` once `amp2`
   *  leaves the chain. */
  ampTarget: "amp" | "amp2";
  shelf: T3kBrowseShelf;
  /** null = API default (best-match with a query, trending without). */
  sort: T3kBrowseSort | null;
  query: string;
  gear: string | null;
  calibrated: boolean;
  detail: T3kTone | null;
}

/** One list (= one request minus its page), as far as it has been paged. */
export interface T3kListState {
  /** Highest page answered; 0 before the first response. */
  page: number;
  totalPages: number;
  total: number;
  tones: T3kTone[];
  error: string | null;
  /** The page in flight; never more than one per list. */
  loadingPage: number | null;
}

/** A tone's downloadable files (session memory only — ToS: no persistent
 *  catalog cache). */
export interface T3kFilesState {
  models: T3kModel[] | null;
  error: string | null;
  loading: boolean;
}

export type T3kLoadPhase = "resolving" | "downloading" | "loading" | "failed";

/** One-click load of a tone, in flight or failed. Absent = idle (or done:
 *  "Loaded" is derived from what the target slot holds). */
export interface T3kRowLoad {
  phase: T3kLoadPhase;
  target: T3kTarget;
  modelId: number | null;
  error: string | null;
}

export interface AppState {
  /** False until the first getUiState() resolves — render skeletons before that. */
  ready: boolean;

  /* mirrored plugin state */
  chainOrder: BlockId[];
  /** Row lengths partitioning `chainOrder`; `[]` = auto (see `rowsFor`). */
  chainRows: number[];
  model: ModelInfo | null;
  /** Engine B's loaded capture (docs/STEREO.md §1 plumbing, now driven by the
   *  `amp2` chain block — docs/SPLIT.md §1) — tracked regardless of whether
   *  `amp2` is in the chain, since clearing model A must not clear B. */
  modelB: ModelInfo | null;
  ir: IrInfo | null;
  /** Files a restored session / preset wanted and could not get (bridge types). */
  missingMedia: MissingMedia;
  models: FileEntry[];
  irs: FileEntry[];
  presets: PresetInfo[];
  currentPresetName: string;
  ab: AbState;
  t3k: T3kState;
  /** Always three entries, indexed by slot — see `normaliseFxSlots`. */
  fxSlots: FxSlotState[];
  fxSupported: boolean;
  /**
   * The window, and what it is allowed to become. Mirrored here rather than read
   * off the bridge per use because it is low-rate structural state, like
   * `fxSlots`: it changes when the user drags the grip or the host reopens us,
   * not per frame. The grip itself still reads it with `getState()` mid-drag so
   * a drag re-renders nothing.
   */
  editorSize: EditorSizeLimits;
  /** Which slot's plugin is drawn inside our window, and how big it turned out. */
  fxEmbed: FxEmbedState;
  /**
   * Which fx panel asked for the current embed. UI-local, and deliberately not
   * derived from `fxEmbed.slot`: a mount request is in flight for a while before
   * the event that confirms it, and the panel that asked can unmount in between —
   * which is exactly the window in which the embed would be leaked.
   */
  fxEmbedRequestedBy: FxSlotIndex | -1;

  /* UI-local state */
  selected: BlockId | null;
  tone: T3kToneResult | null;
  /** modelId → progress in [0,1] while a TONE3000 download is in flight. */
  downloads: Record<number, number>;
  /** modelId → what that download is, for the completion handler. */
  t3kPending: Record<number, T3kPendingDownload>;
  /**
   * toneId → paths this session downloaded from that tone. Session-scoped on
   * purpose: nothing on disk records which tone a file came from, so the
   * drawer's "IN CHAIN" pill can only be honest about downloads it saw.
   */
  t3kDownloaded: Record<number, string[]>;
  browser: T3kBrowserState;
  /** listKey → pages loaded so far (see `t3kListKey`). In-memory, capped. */
  t3kLists: Record<string, T3kListState>;
  /** listKey → last scroll offset of that list. */
  t3kScroll: Record<string, number>;
  /** toneId → optimistic favorite flag, layered over server rows. */
  t3kFavOverrides: Record<number, boolean>;
  t3kFiles: Record<number, T3kFilesState>;
  /** Library files that carry a TONE3000 sidecar (`t3kInstalled`). */
  t3kInstalled: T3kInstalledFile[];
  /** toneId → one-click load state. */
  t3kLoads: Record<number, T3kRowLoad>;
  toasts: Toast[];
}

export interface AppActions {
  hydrate(): Promise<void>;

  /* chain */
  setChainOrder(order: BlockId[], rows: number[]): void;
  /** Adds the lowest instance of `id`'s kind that is not already in the chain;
   *  no-op once the kind is exhausted. By default it appends to the end of the
   *  last row; `at` instead inserts at that flat index, growing that row. */
  addBlock(id: BlockId, at?: { index: number; row: number }): void;
  /** Removes exactly that instance token — never its siblings. */
  removeBlock(id: BlockId): void;
  selectBlock(id: BlockId | null): void;

  /* library / presets — thin wrappers that surface errors as toasts */
  loadModel(path: string): Promise<void>;
  clearModel(): Promise<void>;
  /** Engine B (docs/STEREO.md §1 plumbing; model-B UI now lives on the `amp2`
   *  block, docs/SPLIT.md §5). "Use model A" is just `loadModelB(model.path)`
   *  from the caller — there is no separate action for it. */
  loadModelB(path: string): Promise<void>;
  clearModelB(): Promise<void>;
  importModel(): Promise<void>;
  loadIr(path: string): Promise<void>;
  clearIr(): Promise<void>;
  importIr(): Promise<void>;
  loadPreset(path: string): Promise<void>;
  savePresetAs(): Promise<void>;
  deletePreset(path: string): Promise<void>;
  setPresetFavorite(path: string, favorite: boolean): Promise<void>;
  abCapture(slot: AbSlot): Promise<void>;
  abRecall(slot: AbSlot): Promise<void>;

  /* external AudioUnit slots — the outcome always arrives as `fxSlotChanged`,
     never as the return value, so none of these three writes `fxSlots`. */
  loadFxPlugin(slot: FxSlotIndex, identifier: string): Promise<void>;
  clearFxPlugin(slot: FxSlotIndex): Promise<void>;
  openFxEditor(slot: FxSlotIndex): Promise<void>;
  /** Draw the plugin's editor inside our window; falls back to its own window
   *  when the host refuses (it does not fit the display). */
  embedFxEditor(slot: FxSlotIndex): Promise<void>;
  /** -1 to the bridge: unmounts whatever is embedded. */
  unembedFxEditor(): Promise<void>;

  /* window */
  resizeEditor(width: number, height: number): void;

  /* TONE3000 */
  t3kConfigure(): Promise<void>;
  t3kSignOut(): Promise<void>;
  t3kStartSelectFlow(): Promise<void>;
  /** `toneId` credits the download to a browser row; omit it for the select flow. */
  t3kDownloadModel(model: T3kModel, toneId?: number): Promise<void>;
  dismissTone(): void;
  /** `target` defaults to the cab for IRs, and for models to `amp2` when the
   *  Amp 2 panel is the selected block, else `amp`. Keeps the rest of the
   *  remembered browser state; a kind change resets gear (the gear sets differ)
   *  and closes a detail level of the other format. */
  openT3kBrowser(kind: T3kBrowseKind, target?: T3kTarget): void;
  closeT3kBrowser(): void;
  patchT3kBrowser(
    patch: Partial<
      Pick<T3kBrowserState, "kind" | "shelf" | "sort" | "query" | "gear" | "calibrated">
    >,
  ): void;
  setT3kAmpTarget(target: "amp" | "amp2"): void;
  setT3kDetail(tone: T3kTone | null): void;
  setT3kScroll(key: string, top: number): void;
  /** Fetch `page` of the current list. No-op while that list has a page in
   *  flight; page 1 restarts the list (retry / first load). */
  t3kFetchPage(page: number): Promise<void>;
  t3kToggleFavorite(tone: T3kTone): void;
  t3kListFiles(tone: Pick<T3kTone, "id" | "format">, force?: boolean): Promise<T3kFilesState>;
  /** One-click load: resolve files, pick the default (or `model`), reuse the
   *  library copy or download it, then load into the tone's target. One in
   *  flight per tone; the latest pick per target wins the slot. */
  t3kLoadTone(tone: T3kTone, model?: T3kModel): Promise<void>;
  t3kRefreshInstalled(): Promise<void>;
  t3kOpenExternal(url: string): Promise<void>;

  /* toasts */
  toast(message: string, kind?: ToastKind, duration?: number): number;
  dismissToast(id: number): void;
}

export type Store = AppState & AppActions;

/**
 * Nothing is ever auto-selected: the panel is a click-summoned overlay, so a
 * hydrate or a chain change out of C++ must not open one. A selection only
 * survives while its own token is still in the chain.
 */
function keepSelection(
  order: readonly BlockId[],
  current: BlockId | null,
): BlockId | null {
  return current !== null && order.includes(current) ? current : null;
}

function toBlockIds(tokens: string[]): BlockId[] {
  return tokens.filter(isBlockId);
}

/**
 * Row lengths are only meaningful as an exact partition of the order: every row
 * holds at least one block and the lengths sum to the chain length. Anything
 * else — including rows for a chain that has since changed — is stored as `[]`,
 * which means auto. Never collapsed into a single row: a 24-block lane cannot be
 * framed at the board's minimum zoom.
 */
function normaliseChainRows(
  order: readonly BlockId[],
  rows: readonly number[],
): number[] {
  if (order.length === 0 || rows.length === 0) return [];
  let total = 0;
  for (const length of rows) {
    if (!Number.isInteger(length) || length < 1) return [];
    total += length;
  }
  return total === order.length ? [...rows] : [];
}

/** How many blocks an auto-wrapped row holds. */
export const ROW_WRAP = 6;

/**
 * The rows to draw: the published partition when there is one, otherwise a wrap
 * at ROW_WRAP. Pure, and deliberately NOT published back — an auto chain stays
 * auto until the user rearranges it, so resizing the window never rewrites state.
 */
export function rowsFor(
  order: readonly BlockId[],
  chainRows: readonly number[],
): number[] {
  if (chainRows.length > 0) return [...chainRows];
  const rows: number[] = [];
  for (let i = 0; i < order.length; i += ROW_WRAP)
    rows.push(Math.min(ROW_WRAP, order.length - i));
  return rows;
}

/** The lowest instance of `kind` not already in the chain; null when it has none
 *  left (a singleton that is already there, or all three instances used).
 *  split/lane2/mix are single-token singletons exactly like an fx slot — none
 *  of the three ever reaches this through `BLOCK_BASES` (they are deliberately
 *  excluded from the generic picker, docs/SPLIT.md §1), but `kindOf()` can
 *  still hand one back here from `addBlock`/`removeBlock`, so the type must
 *  be handled regardless. */
function freeInstance(
  kind: BaseBlockId,
  order: readonly BlockId[],
): BlockId | null {
  const tokens: readonly BlockId[] =
    isFxBlockId(kind) || isStructureBlockId(kind)
      ? [kind]
      : instanceTokensOfKind(kind);
  return tokens.find((token) => !order.includes(token)) ?? null;
}

/** Appending grows the last row; an auto chain stays auto. */
function rowsWithAppend(rows: readonly number[]): number[] {
  if (rows.length === 0) return [];
  const next = [...rows];
  next[next.length - 1] += 1;
  return next;
}

/** Inserting grows `row`; an auto chain stays auto (it re-wraps by itself). */
function rowsWithInsertAt(rows: readonly number[], row: number): number[] {
  if (rows.length === 0) return [];
  const next = [...rows];
  next[Math.min(Math.max(row, 0), next.length - 1)] += 1;
  return next;
}

/** Removing shrinks the row that owned the block, and drops it when it empties. */
function rowsWithout(rows: readonly number[], index: number): number[] {
  if (rows.length === 0) return [];
  const next = [...rows];
  let start = 0;
  for (let row = 0; row < next.length; row += 1) {
    if (index < start + next[row]!) {
      next[row] -= 1;
      break;
    }
    start += next[row]!;
  }
  return next.filter((length) => length > 0);
}

function emptyFxSlot(slot: FxSlotIndex): FxSlotState {
  return {
    slot,
    identifier: "",
    name: "",
    manufacturer: "",
    occupied: false,
    missing: false,
    live: false,
    loading: false,
    latencySamples: 0,
    error: "",
    hasEditor: false,
    params: [],
  };
}

/**
 * Three entries, always. Every consumer indexes this array by slot, so a short
 * (or absent) payload from a build without plugin hosting must not turn into
 * `undefined` reads scattered across the panel and the board.
 */
function normaliseFxSlots(slots: readonly FxSlotState[] | undefined): FxSlotState[] {
  return [0, 1, 2].map(
    (i) => slots?.[i] ?? emptyFxSlot(i as FxSlotIndex),
  );
}

let nextToastId = 1;

const initialState: AppState = {
  ready: false,
  chainOrder: [],
  chainRows: [],
  model: null,
  modelB: null,
  ir: null,
  missingMedia: { model: null, modelB: null, ir: null },
  models: [],
  irs: [],
  presets: [],
  currentPresetName: "",
  ab: { activeSlot: 0, aHasState: false, bHasState: false },
  t3k: { configured: false, authenticated: false, username: null },
  fxSlots: normaliseFxSlots(undefined),
  // Assumed off until hydration says otherwise: claiming hosting works and then
  // withdrawing it reads as a bug, the other way round reads as a slow load.
  fxSupported: false,
  // Placeholder for the ~one frame before hydration. The minimum is the real
  // one (theme/tokens.css); the maximum is deliberately huge, because guessing
  // small here would let the grip refuse a size the display can actually take.
  editorSize: {
    width: 1280,
    height: 800,
    minWidth: 1000,
    minHeight: 600,
    maxWidth: 8192,
    maxHeight: 8192,
  },
  fxEmbed: { slot: -1, width: 0, height: 0, error: "" },
  fxEmbedRequestedBy: -1,
  selected: null,
  tone: null,
  downloads: {},
  t3kPending: {},
  t3kDownloaded: {},
  browser: {
    open: false,
    kind: "models",
    ampTarget: "amp",
    shelf: "all",
    sort: null,
    query: "",
    gear: null,
    calibrated: false,
    detail: null,
  },
  t3kLists: {},
  t3kScroll: {},
  t3kFavOverrides: {},
  t3kFiles: {},
  t3kInstalled: [],
  t3kLoads: {},
  toasts: [],
};

export const useStore = create<Store>()((set, get) => {
  /** Publish chain order + rows to C++ — the ONLY place that writes them, and
   *  once per gesture. */
  const publish = (order: BlockId[], rows: number[]): void => {
    void bridge.setChainOrder(order, rows);
  };

  const reportError = (message: string | undefined): boolean => {
    if (!message) return false;
    get().toast(message, "error", 6000);
    return true;
  };

  return {
    ...initialState,

    async hydrate() {
      const state: UiState = await bridge.getUiState();
      const order = toBlockIds(state.chainOrder);
      set({
        ready: true,
        chainOrder: order,
        chainRows: normaliseChainRows(order, state.chainRows ?? []),
        model: state.model,
        // `?? null`: a C++ build that predates docs/STEREO.md must still hydrate.
        modelB: state.modelB ?? null,
        ir: state.ir,
        // `??`: a C++ build that predates missing-file reporting must still hydrate.
        missingMedia: state.missingMedia ?? { model: null, modelB: null, ir: null },
        models: state.models,
        irs: state.irs,
        presets: state.presets,
        currentPresetName: state.currentPresetName,
        ab: state.ab,
        t3k: state.t3k,
        fxSlots: normaliseFxSlots(state.fxSlots),
        fxSupported: state.fxSupported,
        // `?? initial` rather than a hard read: a C++ build that predates these
        // two fields must hydrate the rest of the snapshot, not crash the grip.
        editorSize: state.editorSize ?? initialState.editorSize,
        fxEmbed: state.fxEmbed ?? initialState.fxEmbed,
        selected: keepSelection(order, get().selected),
      });
    },

    /* ─────────────────────────────── chain ───────────────────────────── */

    setChainOrder(order, rows) {
      // The UI must never COMMIT a malformed split-path structure (docs/SPLIT.md
      // §5) — C++'s own `sanitizeStructure` is the backstop, not the norm, so
      // this mirrors it before publishing: a dangling split/lane-before-mix
      // flattens to serial here too, client-side, rather than round-tripping
      // through C++ to find out.
      const sanitized = sanitizeStructure(order);
      const chainRows = normaliseChainRows(sanitized, rows);
      set({
        chainOrder: sanitized,
        chainRows,
        selected: keepSelection(sanitized, get().selected),
      });
      publish(sanitized, chainRows);
    },

    addBlock(id, at) {
      const { chainOrder, chainRows } = get();
      // The cap is C++'s (chain::maxChainLength), and it is enforced HERE for the
      // same reason `canAddSplit` already enforces it: past it `sanitiseOrderAndRows`
      // drops the entries that did not fit, so a 25th block would come straight back
      // out of the `chainChanged` round-trip and the user would watch the block they
      // just placed disappear. There are more block ids (28) than the chain can
      // hold (24), so this is reachable, not theoretical.
      if (chainOrder.length >= MAX_CHAIN_LENGTH) return;
      const token = freeInstance(kindOf(id), chainOrder);
      if (token === null) return;
      const index =
        at === undefined
          ? chainOrder.length
          : Math.min(Math.max(at.index, 0), chainOrder.length);
      const order = [
        ...chainOrder.slice(0, index),
        token,
        ...chainOrder.slice(index),
      ];
      const rows =
        at === undefined
          ? rowsWithAppend(chainRows)
          : rowsWithInsertAt(chainRows, at.row);
      // Adding always opens the new block's panel: the user asked for it, and
      // it is the only block on the board whose settings they have not seen.
      set({ chainOrder: order, chainRows: rows, selected: token });
      publish(order, rows);
    },

    removeBlock(id) {
      const { chainOrder, chainRows, selected } = get();
      const index = chainOrder.indexOf(id);
      if (index < 0) return;
      // SPLIT and MIX are one shape, not one block (docs/SPLIT.md §5): removing
      // either removes the REGION — lane A's blocks then lane B's, inline where the
      // split stood, which is exactly the flat order minus the three structural
      // tokens. Filtering out the single id instead would commit `..., split, ...,
      // lane2, ...`, a malformed structure the UI must never ASK C++ for even
      // though C++ would flatten it right back. This is the panel's path; the
      // board's own structural remove keeps its explicit row layout (rowsWithoutRun),
      // while here the layout degrades to auto because rows cannot describe an order
      // that just lost three entries.
      const structural = isStructureBlockId(id);
      const order = structural
        ? flattenStructure(chainOrder)
        : chainOrder.filter((block) => block !== id);
      const rows = structural ? [] : rowsWithout(chainRows, index);
      set({
        chainOrder: order,
        chainRows: rows,
        selected: keepSelection(order, selected),
      });
      publish(order, rows);
    },

    // There is deliberately no `moveBlock(id, index)`: the board's reorder drag
    // commits the whole resulting order through `setChainOrder`, so a gesture is
    // exactly one write to C++. A second mutation path would make that
    // guarantee depend on the caller.

    selectBlock(id) {
      if (id !== null && !get().chainOrder.includes(id)) return;
      set({ selected: id });
    },

    /* ──────────────────────── library / presets ──────────────────────── */

    async loadModel(path) {
      const res = await bridge.loadModel(path);
      if (!reportError(res.error)) {
        const name = get().models.find((m) => m.path === path)?.name ?? "";
        if (name) get().toast(`Loaded ${name}`, "success", 2500);
      }
    },

    async clearModel() {
      await bridge.clearModel();
    },

    async loadModelB(path) {
      const res = await bridge.loadModelB(path);
      if (!reportError(res.error)) {
        const name = get().models.find((m) => m.path === path)?.name ?? "";
        if (name) get().toast(`Loaded ${name} as Model B`, "success", 2500);
      }
    },

    async clearModelB() {
      await bridge.clearModelB();
    },

    // `importModel`/`importIr` install *and* load the file on the C++ side, so
    // there is no loadModel/loadIr call here: a second one would re-parse the
    // capture on the message thread and fire a second latency change at the host.
    async importModel() {
      const res = await bridge.importModel();
      if (reportError(res.error)) return;
      if (res.name) get().toast(`Loaded ${res.name}`, "success", 2500);
    },

    async loadIr(path) {
      const res = await bridge.loadIr(path);
      reportError(res.error);
    },

    async clearIr() {
      await bridge.clearIr();
    },

    async importIr() {
      const res = await bridge.importIr();
      reportError(res.error);
    },

    async loadPreset(path) {
      const res = await bridge.loadPreset(path);
      reportError(res.error);
    },

    async savePresetAs() {
      const res = await bridge.savePresetAs();
      reportError(res.error);
    },

    async deletePreset(path) {
      const res = await bridge.deletePreset(path);
      reportError(res.error);
    },

    async setPresetFavorite(path, favorite) {
      await bridge.setPresetFavorite(path, favorite);
    },

    async abCapture(slot) {
      await bridge.abCapture(slot);
      get().toast(`Captured to ${slot === 0 ? "A" : "B"}`, "success", 2000);
    },

    async abRecall(slot) {
      await bridge.abRecall(slot);
    },

    /* ────────────────────── external AudioUnit slots ─────────────────── */

    // Resolving only means the request was accepted; the instance is created on
    // another thread and reports itself through `fxSlotChanged`. Anything that
    // waits on this promise for a *loaded* plugin is waiting on the wrong thing.
    async loadFxPlugin(slot, identifier) {
      const res = await bridge.fxLoad(slot, identifier);
      reportError(res.error);
    },

    async clearFxPlugin(slot) {
      await bridge.fxClear(slot);
    },

    async openFxEditor(slot) {
      const res = await bridge.fxOpenEditor(slot);
      reportError(res.error);
    },

    // Refusal is not an error the user can act on — the plugin's editor simply
    // does not fit the display — so it is not a dead end either: open the
    // plugin's own window instead and say why. The bridge also broadcasts the
    // refusal as `fxEmbedChanged` (slot -1 + error); that path only updates
    // state, so the toast is not doubled.
    async embedFxEditor(slot) {
      // Claimed before the await, so a panel that unmounts while the request is
      // in flight still knows the embed is its to tear down.
      set({ fxEmbedRequestedBy: slot });
      const res = await bridge.fxSetEmbedSlot(slot);
      if (!res.error) return;
      set({ fxEmbedRequestedBy: -1 });
      get().toast(`${res.error} Opening it in its own window.`, "warning", 6000);
      const opened = await bridge.fxOpenEditor(slot);
      reportError(opened.error);
    },

    async unembedFxEditor() {
      set({ fxEmbedRequestedBy: -1 });
      await bridge.fxSetEmbedSlot(-1);
    },

    /* ─────────────────────────────── window ──────────────────────────── */

    // Fire-and-forget: `editorSizeChanged` is the single source of truth for the
    // size, including for the sizes we ask for ourselves.
    resizeEditor(width, height) {
      void bridge.setEditorSize(width, height);
    },

    /* ─────────────────────────── TONE3000 ────────────────────────────── */

    async t3kConfigure() {
      const res = await bridge.t3kConfigure();
      reportError(res.error);
    },

    async t3kSignOut() {
      await bridge.t3kSignOut();
    },

    async t3kStartSelectFlow() {
      await bridge.t3kStartSelectFlow();
    },

    async t3kDownloadModel(model, toneId) {
      set((s) => ({
        downloads: { ...s.downloads, [model.id]: 0 },
        t3kPending: {
          ...s.t3kPending,
          [model.id]: {
            name: model.name,
            kind: model.kind,
            toneId: toneId ?? null,
          },
        },
      }));
      await bridge.t3kDownloadModel(model);
    },

    dismissTone() {
      set({ tone: null });
    },

    openT3kBrowser(kind, target) {
      const s = get();
      const b = s.browser;
      const ampTarget: "amp" | "amp2" =
        target === "amp" || target === "amp2"
          ? target
          : kind === "models" && s.selected === "amp2"
            ? "amp2"
            : kind === "models"
              ? "amp"
              : b.ampTarget;
      const kindChanged = b.kind !== kind;
      const format = kind === "irs" ? "ir" : "nam";
      set({
        browser: {
          ...b,
          open: true,
          kind,
          ampTarget,
          gear: kindChanged ? null : b.gear,
          detail: b.detail && b.detail.format !== format ? null : b.detail,
        },
      });
      void get().t3kRefreshInstalled();
    },

    closeT3kBrowser() {
      set((s) => ({ browser: { ...s.browser, open: false } }));
    },

    patchT3kBrowser(patch) {
      set((s) => {
        const b = s.browser;
        const next: T3kBrowserState = { ...b, ...patch };
        if (patch.kind !== undefined && patch.kind !== b.kind && patch.gear === undefined)
          next.gear = null;
        const hadQuery = b.query.trim() !== "";
        const hasQuery = next.query.trim() !== "";
        // A new search drops a "trending" pick (relevance is what a search
        // wants); an emptied one drops "best-match" (meaningless without text).
        if (patch.sort === undefined) {
          if (!hadQuery && hasQuery && next.sort === "trending") next.sort = null;
          if (!hasQuery && next.sort === "best-match") next.sort = null;
        }
        return { browser: next };
      });
    },

    setT3kAmpTarget(ampTarget) {
      set((s) => ({ browser: { ...s.browser, ampTarget } }));
    },

    setT3kDetail(detail) {
      set((s) => ({ browser: { ...s.browser, detail } }));
    },

    setT3kScroll(key, top) {
      set((s) => ({ t3kScroll: { ...s.t3kScroll, [key]: top } }));
    },

    async t3kFetchPage(page) {
      const browser = get().browser;
      const key = t3kListKey(browser);
      const current = get().t3kLists[key];
      if (current?.loadingPage != null) return;

      set((s) => ({
        t3kLists: trimLists(
          {
            ...s.t3kLists,
            [key]: {
              ...(current ?? EMPTY_LIST),
              loadingPage: page,
              error: null,
            },
          },
          key,
        ),
      }));

      const res = await bridge.t3kBrowse(t3kRequestOf(browser, page));

      set((s) => {
        const list = s.t3kLists[key];
        // Dropped (cap) or restarted meanwhile: this answer belongs to nobody.
        if (!list || list.loadingPage !== page) return {};
        if (res.error !== undefined) {
          return {
            t3kLists: {
              ...s.t3kLists,
              [key]: { ...list, loadingPage: null, error: res.error },
            },
          };
        }
        const incoming = res.tones ?? [];
        const seen = new Set(page === 1 ? [] : list.tones.map((t) => t.id));
        const tones =
          page === 1
            ? incoming
            : [...list.tones, ...incoming.filter((t) => !seen.has(t.id))];
        return {
          t3kLists: {
            ...s.t3kLists,
            [key]: {
              page: res.page ?? page,
              totalPages: res.totalPages ?? 1,
              total: res.total ?? tones.length,
              tones,
              error: null,
              loadingPage: null,
            },
          },
        };
      });
    },

    t3kToggleFavorite(tone) {
      // One request per tone at a time: a PUT and a DELETE fired back to back
      // travel on separate threads and can reach the server in either order,
      // leaving the heart showing the opposite of what is stored.
      if (favoritesInFlight.has(tone.id)) return;
      favoritesInFlight.add(tone.id);

      const overrides = get().t3kFavOverrides;
      const next = !(overrides[tone.id] ?? tone.favorited);
      set((s) => ({ t3kFavOverrides: { ...s.t3kFavOverrides, [tone.id]: next } }));
      void bridge.t3kSetFavorite(tone.id, next).then((res) => {
        favoritesInFlight.delete(tone.id);
        if (res.error) {
          set((s) => ({
            t3kFavOverrides: { ...s.t3kFavOverrides, [tone.id]: !next },
          }));
          get().toast(res.error, "error", 6000);
          return;
        }
        // The favorites shelf changed in a way no cached page knows about; the
        // one on screen stays put (rows vanishing under the cursor is worse).
        set((s) => ({ t3kLists: dropShelfLists(s.t3kLists, "favorites", t3kListKey(s.browser)) }));
      });
    },

    t3kListFiles(tone, force = false) {
      const cached = get().t3kFiles[tone.id];
      const inflight = filesInFlight.get(tone.id);
      if (inflight) return inflight;
      if (cached && cached.models && !force) return Promise.resolve(cached);

      set((s) => ({
        t3kFiles: {
          ...s.t3kFiles,
          [tone.id]: { models: null, error: null, loading: true },
        },
      }));
      const request: Promise<T3kFilesState> = bridge
        .t3kListModels(tone.id, tone.format)
        .then((res): T3kFilesState => {
          const result: T3kFilesState = {
            models: res.error ? null : (res.models ?? []),
            error: res.error ?? null,
            loading: false,
          };
          // Superseded — the account changed while this was in the air and the
          // map was cleared: the answer belongs to a session that is gone.
          if (filesInFlight.get(tone.id) !== request) return result;
          filesInFlight.delete(tone.id);
          set((s) => ({ t3kFiles: { ...s.t3kFiles, [tone.id]: result } }));
          return result;
        });
      filesInFlight.set(tone.id, request);
      return request;
    },

    async t3kLoadTone(tone, explicit) {
      const existing = get().t3kLoads[tone.id];
      if (existing && existing.phase !== "failed") return;

      const target = targetForTone(get(), tone);
      latestPick[target] = tone.id;
      const setLoad = (load: T3kRowLoad | null): void =>
        set((s) => {
          const t3kLoads = { ...s.t3kLoads };
          if (load) t3kLoads[tone.id] = load;
          else delete t3kLoads[tone.id];
          return { t3kLoads };
        });
      const fail = (error: string, modelId: number | null): void =>
        setLoad({ phase: "failed", target, modelId, error });

      let model = explicit;
      if (!model) {
        setLoad({ phase: "resolving", target, modelId: null, error: null });
        const files = await get().t3kListFiles(tone);
        if (files.error) return fail(files.error, null);
        model = bestDefaultFile(files.models ?? []) ?? undefined;
        if (!model)
          return fail(
            tone.format === "ir"
              ? "This tone has no downloadable IRs."
              : "This tone has no A2 captures to load.",
            null,
          );
      }

      const path = findInstalledPath(get(), model);
      if (path) {
        if (latestPick[target] !== tone.id) return setLoad(null);
        setLoad({ phase: "loading", target, modelId: model.id, error: null });
        const error = await loadIntoTarget(target, path, model.name);
        if (error) return fail(error, model.id);
        set((s) => ({ t3kDownloaded: creditPath(s.t3kDownloaded, tone.id, path) }));
        return setLoad(null);
      }

      setLoad({ phase: "downloading", target, modelId: model.id, error: null });
      const picked = model;
      set((s) => ({
        downloads: { ...s.downloads, [picked.id]: s.downloads[picked.id] ?? 0 },
        t3kPending: {
          ...s.t3kPending,
          [picked.id]: { name: picked.name, kind: picked.kind, toneId: tone.id, target },
        },
      }));
      await bridge.t3kDownloadModel(picked, tone);
    },

    async t3kRefreshInstalled() {
      const res = await bridge.t3kInstalled();
      set({ t3kInstalled: res.files ?? [] });
    },

    async t3kOpenExternal(url) {
      const res = await bridge.openExternal(url);
      reportError(res.error);
    },

    /* ───────────────────────────── toasts ────────────────────────────── */

    toast(message, kind = "info", duration = 4000) {
      const id = nextToastId++;
      set((s) => ({ toasts: [...s.toasts, { id, kind, message, duration }] }));
      return id;
    },

    dismissToast(id) {
      set((s) => ({ toasts: s.toasts.filter((t) => t.id !== id) }));
    },
  };
});

/* ───────────────────────── bridge event wiring ─────────────────────────── */

/**
 * Load a finished download into the slot its `kind` names — a capture goes to
 * the amp, an IR to the cab. Goes to the bridge directly rather than through
 * `loadModel`/`loadIr` so both outcomes report with the downloaded file's name,
 * which is known here and not yet guaranteed to be in the library snapshot.
 */
async function autoLoad(
  path: string,
  pending: T3kPendingDownload | undefined,
): Promise<void> {
  const { toast, models } = useStore.getState();
  const res =
    pending?.kind === "wav"
      ? await bridge.loadIr(path)
      : await bridge.loadModel(path);
  if (res.error) {
    toast(res.error, "error", 6000);
    return;
  }
  const name = pending?.name ?? models.find((m) => m.path === path)?.name;
  toast(name ? `Loaded ${name}` : "Loaded", "success", 2500);
}

/* ─────────────────────────── TONE3000 helpers ─────────────────────────── */

const EMPTY_LIST: T3kListState = {
  page: 0,
  totalPages: 1,
  total: 0,
  tones: [],
  error: null,
  loadingPage: null,
};

/** Lists kept in memory at once (each up to a few pages of 25 rows). */
const MAX_LISTS = 12;

/** toneId → its in-flight `t3kListModels`, so a row click and the detail
 *  level never ask twice. */
const filesInFlight = new Map<number, Promise<T3kFilesState>>();

/** Tones with a favorite toggle on its way to the server. */
const favoritesInFlight = new Set<number>();

/** Last tone picked per target: a download that finishes after a newer pick
 *  only lands in the library, it does not steal the slot back. */
const latestPick: Record<T3kTarget, number> = { amp: 0, amp2: 0, cab: 0 };

/** Calibration is a capture-only search filter; it means nothing elsewhere. */
function effectiveCalibrated(b: T3kBrowserState): boolean {
  return b.calibrated && b.kind === "models" && b.shelf === "all";
}

/** The request minus its page — two requests sharing this describe one list. */
export function t3kListKey(b: T3kBrowserState): string {
  return JSON.stringify([
    b.kind,
    b.shelf,
    b.sort,
    b.query.trim(),
    b.gear,
    effectiveCalibrated(b),
  ]);
}

export function t3kRequestOf(b: T3kBrowserState, page: number): T3kBrowseRequest {
  return {
    kind: b.kind,
    shelf: b.shelf,
    sort: b.sort,
    query: b.query.trim(),
    gear: b.gear,
    calibrated: effectiveCalibrated(b),
    page,
  };
}

/** Keeps the newest MAX_LISTS lists (insertion order), always keeping `keep`. */
function trimLists(
  lists: Record<string, T3kListState>,
  keep: string,
): Record<string, T3kListState> {
  const keys = Object.keys(lists);
  if (keys.length <= MAX_LISTS) return lists;
  const next = { ...lists };
  for (const key of keys.slice(0, keys.length - MAX_LISTS))
    if (key !== keep) delete next[key];
  return next;
}

function dropShelfLists(
  lists: Record<string, T3kListState>,
  shelf: T3kBrowseShelf,
  keep: string,
): Record<string, T3kListState> {
  const next = { ...lists };
  for (const key of Object.keys(next)) {
    const parsed = JSON.parse(key) as unknown[];
    if (parsed[1] === shelf && key !== keep) delete next[key];
  }
  return next;
}

/** The engine/slot a load from the browser lands in. */
export function selectT3kTarget(s: Store): T3kTarget {
  if (s.browser.kind === "irs") return "cab";
  return s.browser.ampTarget === "amp2" && s.chainOrder.includes("amp2")
    ? "amp2"
    : "amp";
}

function targetForTone(s: Store, tone: T3kTone): T3kTarget {
  if (tone.format === "ir") return "cab";
  return s.browser.ampTarget === "amp2" && s.chainOrder.includes("amp2")
    ? "amp2"
    : "amp";
}

/** Mirrors Tone3000Client's slugify: how a downloaded file ends up named. */
export function t3kSlug(name: string): string {
  const slug = name
    .trim()
    .toLowerCase()
    .replace(/[^\p{L}\p{N}]+/gu, "-")
    .replace(/^-+|-+$/g, "");
  return slug || "model";
}

/** Default file of a tone: NAM prefers `standard`, else the first; IR the first. */
export function bestDefaultFile(models: readonly T3kModel[]): T3kModel | null {
  return models.find((m) => m.kind === "nam" && m.size === "standard") ?? models[0] ?? null;
}

/** Library path of a TONE3000 file, if it is installed: the sidecar's modelId
 *  first, then the file name the downloader gives it. */
export function findInstalledPath(s: AppState, model: T3kModel): string | null {
  const shelf = model.kind === "wav" ? s.irs : s.models;
  const bySidecar = s.t3kInstalled.find((f) => f.modelId === model.id);
  if (bySidecar && shelf.some((f) => f.path === bySidecar.path)) return bySidecar.path;
  const slug = t3kSlug(model.name);
  return shelf.find((f) => f.name === slug || f.name === model.name)?.path ?? null;
}

/** What each target slot currently holds. */
export function loadedPathOf(s: AppState, target: T3kTarget): string | null {
  if (target === "cab") return s.ir?.path ?? null;
  if (target === "amp2") return s.modelB?.path ?? null;
  return s.model?.path ?? null;
}

/** Every library path known to come from `toneId` (sidecars + this session). */
export function tonePaths(s: AppState, toneId: number): string[] {
  return [
    ...s.t3kInstalled.filter((f) => f.toneId === toneId).map((f) => f.path),
    ...(s.t3kDownloaded[toneId] ?? []),
  ];
}

/** Targets whose slot currently holds a file of `toneId`. */
export function loadedTargetsOf(s: AppState, toneId: number): T3kTarget[] {
  const paths = tonePaths(s, toneId);
  if (paths.length === 0) return [];
  return (["amp", "amp2", "cab"] as const).filter((t) => {
    const loaded = loadedPathOf(s, t);
    return loaded !== null && paths.includes(loaded);
  });
}

export function decorateTone(
  tone: T3kTone,
  overrides: Record<number, boolean>,
): T3kTone {
  const override = overrides[tone.id];
  if (override === undefined || override === tone.favorited) return tone;
  return {
    ...tone,
    favorited: override,
    favoritesCount: Math.max(0, tone.favoritesCount + (override ? 1 : -1)),
  };
}

function creditPath(
  credited: Record<number, string[]>,
  toneId: number,
  path: string,
): Record<number, string[]> {
  const paths = credited[toneId] ?? [];
  return paths.includes(path) ? credited : { ...credited, [toneId]: [...paths, path] };
}

export const T3K_TARGET_LABELS: Record<T3kTarget, string> = {
  amp: "Amp",
  amp2: "Amp 2",
  cab: "Cab",
};

/** Loads into one slot; resolves with an error message or null. */
async function loadIntoTarget(
  target: T3kTarget,
  path: string,
  name: string,
): Promise<string | null> {
  const res =
    target === "cab"
      ? await bridge.loadIr(path)
      : target === "amp2"
        ? await bridge.loadModelB(path)
        : await bridge.loadModel(path);
  if (res.error) return res.error;
  useStore
    .getState()
    .toast(
      target === "amp2" ? `Loaded ${name} into Amp 2` : `Loaded ${name}`,
      "success",
      2500,
    );
  return null;
}

/** A browser download finished: load it into its target unless a newer pick
 *  took that slot meanwhile, then settle the row. */
async function finishBrowserLoad(
  path: string,
  pending: T3kPendingDownload,
  modelId: number,
): Promise<void> {
  const { toneId, target } = pending;
  if (toneId == null || !target) return;
  const set = useStore.setState;
  const settle = (load: T3kRowLoad | null): void =>
    set((s) => {
      const t3kLoads = { ...s.t3kLoads };
      if (load) t3kLoads[toneId] = load;
      else delete t3kLoads[toneId];
      return { t3kLoads };
    });

  if (latestPick[target] !== toneId) {
    settle(null);
    useStore.getState().toast(`Saved ${pending.name} to your library`, "info", 3000);
    return;
  }
  settle({ phase: "loading", target, modelId, error: null });
  const error = await loadIntoTarget(target, path, pending.name);
  if (error) {
    settle({ phase: "failed", target, modelId, error });
    useStore.getState().toast(error, "error", 6000);
    return;
  }
  settle(null);
}

let wired = false;

/**
 * Subscribe the store to every bridge event. Called once from main.tsx; safe to
 * call again (no-ops). Returns a teardown for tests/HMR.
 */
export function connectStore(): () => void {
  if (wired) return () => {};
  wired = true;

  const set = useStore.setState;
  const get = useStore.getState;

  const unsubs = [
    bridge.on("chainChanged", ({ chainOrder, chainRows }) => {
      const order = toBlockIds(chainOrder);
      set({
        chainOrder: order,
        chainRows: normaliseChainRows(order, chainRows ?? []),
        selected: keepSelection(order, get().selected),
      });
    }),

    bridge.on("libraryChanged", ({ models, irs }) => {
      set({ models, irs });
      // Sidecars are written before this fires, so the scan sees new files.
      void get().t3kRefreshInstalled();
    }),

    bridge.on("presetChanged", ({ presets, currentPresetName, ab }) =>
      set({ presets, currentPresetName, ab }),
    ),

    bridge.on("modelChanged", ({ model }) => set({ model })),

    bridge.on("modelBChanged", ({ modelB }) => set({ modelB })),

    bridge.on("irChanged", ({ ir }) => set({ ir })),

    bridge.on("missingMediaChanged", ({ missingMedia }) => set({ missingMedia })),

    bridge.on("t3kStatus", (t3k) => {
      const before = get().t3k;
      // Another account (or none): every remembered list, file listing and
      // favorite override described the old session.
      if (before.authenticated !== t3k.authenticated || before.username !== t3k.username) {
        // The open detail carries the old account's favorite flag, and a file
        // listing still in the air would land in the cleared map.
        filesInFlight.clear();
        favoritesInFlight.clear();
        set((s) => ({
          t3k,
          t3kLists: {},
          t3kScroll: {},
          t3kFiles: {},
          t3kFavOverrides: {},
          t3kLoads: {},
          browser: { ...s.browser, detail: null },
        }));
      } else set({ t3k });
    }),

    bridge.on("t3kToneSelected", ({ toneId, models }) => {
      if (models.length === 0) {
        // The native UI failed silently here; surface it.
        get().toast("That TONE3000 tone has no downloadable models.", "warning");
        set({ tone: null });
        return;
      }
      set({ tone: { toneId, models } });
    }),

    bridge.on("t3kProgress", ({ modelId, progress }) =>
      set((s) => ({ downloads: { ...s.downloads, [modelId]: progress } })),
    ),

    bridge.on("t3kComplete", ({ modelId, path }) => {
      const pending = get().t3kPending[modelId];
      set((s) => {
        const downloads = { ...s.downloads };
        const t3kPending = { ...s.t3kPending };
        delete downloads[modelId];
        delete t3kPending[modelId];
        const toneId = pending?.toneId;
        const t3kDownloaded =
          toneId == null
            ? s.t3kDownloaded
            : {
                ...s.t3kDownloaded,
                [toneId]: [...(s.t3kDownloaded[toneId] ?? []), path],
              };
        return {
          downloads,
          t3kPending,
          t3kDownloaded,
          tone: null,
          // TONE3000 now lists this tone under Recently used.
          t3kLists: dropShelfLists(s.t3kLists, "downloaded", t3kListKey(s.browser)),
        };
      });
      void get().t3kRefreshInstalled();
      // C++ only downloads + refreshes the library; loading is the UI's call.
      // A browser load names its target; everything else routes by `kind`.
      if (pending?.target) void finishBrowserLoad(path, pending, modelId);
      else void autoLoad(path, pending);
    }),

    bridge.on("fxSlotChanged", ({ slot, state }) => {
      set((s) => {
        const fxSlots = s.fxSlots.slice();
        fxSlots[slot] = state;
        return { fxSlots };
      });
    }),

    // The window is only ever resized by us or by the host reopening the
    // editor, so this is structural state, not a stream — even though a grip
    // drag briefly emits at frame rate. Nothing selects `editorSize` in a
    // render-hot path (the grip reads it with `getState()` mid-drag), so those
    // frames cost a store write and no re-render.
    bridge.on("editorSizeChanged", (editorSize) => set({ editorSize })),

    bridge.on("fxEmbedChanged", (fxEmbed) => set({ fxEmbed })),

    // `fxParamValues` is deliberately NOT wired here. It arrives at meter rate
    // for up to a hundred parameters at once, and a store write would re-render
    // the whole grid every frame; like `meters`, it is consumed straight off the
    // bridge into motion values (features/panel/FxParamGrid.tsx).

    bridge.on("t3kError", ({ message, modelId }) => {
      // A download that failed never sends t3kComplete, so its progress row has
      // to be cleared here or it stays on the tone picker forever.
      if (modelId !== undefined) {
        set((s) => {
          const downloads = { ...s.downloads };
          const t3kPending = { ...s.t3kPending };
          delete downloads[modelId];
          delete t3kPending[modelId];
          // The row that asked for this file shows the failure + Retry.
          const t3kLoads = { ...s.t3kLoads };
          for (const [toneId, load] of Object.entries(t3kLoads))
            if (load.modelId === modelId && load.phase === "downloading")
              t3kLoads[Number(toneId)] = { ...load, phase: "failed", error: message };
          return { downloads, t3kPending, t3kLoads };
        });
      }
      get().toast(message, "error", 6000);
    }),
  ];

  return () => {
    for (const off of unsubs) off();
    wired = false;
  };
}

/* ──────────────────────────────── selectors ────────────────────────────── */

export const selectSelectedBlock = (s: Store): BlockId | null => s.selected;
export const selectChainOrder = (s: Store): BlockId[] => s.chainOrder;

/** The slot behind an fx block; null for the nine built-in blocks. */
export function selectFxSlotFor(block: BlockId | null) {
  return (s: Store): FxSlotState | null => {
    if (block === null) return null;
    const index = fxSlotIndexOf(block);
    return index < 0 ? null : (s.fxSlots[index] ?? null);
  };
}

/** Every identity base, in frozen enum order (= the add-menu's row order). */
export const BLOCK_BASES: readonly BaseBlockId[] = [
  ...BLOCK_KINDS,
  ...FX_BLOCK_IDS,
];

/**
 * Kinds the picker can still add — those with an instance left. A duplicable
 * kind stays offered until all three are in the chain; a singleton drops out as
 * soon as it is there. In frozen enum order (= the add-menu order).
 *
 * Pure, and taken as an argument rather than off the store: a selector that
 * builds an array would hand `useStore` a new snapshot on every render.
 */
export function addableKinds(order: readonly BlockId[]): BaseBlockId[] {
  // A full chain can add nothing at all, whatever instances are still free: C++
  // drops entries past `chain::maxChainLength`, so offering one would be offering a
  // block that vanishes on the way back. Same cap `canAddSplit` applies to the fork.
  if (order.length >= MAX_CHAIN_LENGTH) return [];
  return BLOCK_BASES.filter((base) => freeInstance(base, order) !== null);
}

/** How many instances of `base` the chain currently holds (0…3). */
export function instancesInChain(
  base: BaseBlockId,
  order: readonly BlockId[],
): number {
  const tokens: readonly BlockId[] =
    isFxBlockId(base) || isStructureBlockId(base)
      ? [base]
      : instanceTokensOfKind(base);
  return tokens.reduce((n, token) => (order.includes(token) ? n + 1 : n), 0);
}

export function selectAddableKinds(s: Store): BaseBlockId[] {
  return addableKinds(s.chainOrder);
}
