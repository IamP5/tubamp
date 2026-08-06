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
  type ModelInfo,
  type PresetInfo,
  type T3kBrowseKind,
  type T3kModel,
  type T3kState,
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
}

export interface T3kBrowserState {
  open: boolean;
  kind: T3kBrowseKind;
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
  ir: FileEntry | null;
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
  openT3kBrowser(kind: T3kBrowseKind): void;
  closeT3kBrowser(): void;

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
  browser: { open: false, kind: "models" },
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

    openT3kBrowser(kind) {
      set({ browser: { open: true, kind } });
    },

    closeT3kBrowser() {
      set((s) => ({ browser: { ...s.browser, open: false } }));
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

    bridge.on("libraryChanged", ({ models, irs }) => set({ models, irs })),

    bridge.on("presetChanged", ({ presets, currentPresetName, ab }) =>
      set({ presets, currentPresetName, ab }),
    ),

    bridge.on("modelChanged", ({ model }) => set({ model })),

    bridge.on("modelBChanged", ({ modelB }) => set({ modelB })),

    bridge.on("irChanged", ({ ir }) => set({ ir })),

    bridge.on("t3kStatus", (t3k) => set({ t3k })),

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
        return { downloads, t3kPending, t3kDownloaded, tone: null };
      });
      // C++ only downloads + refreshes the library; loading is the UI's call,
      // and `kind` decides which slot the file belongs in.
      void autoLoad(path, pending);
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
          return { downloads, t3kPending };
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
