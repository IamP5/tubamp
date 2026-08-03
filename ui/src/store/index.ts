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
  type AbSlot,
  type AbState,
  type BlockId,
  type FileEntry,
  type ModelInfo,
  type PresetInfo,
  type T3kModel,
  type T3kState,
  type UiState,
} from "../bridge";
import { BLOCK_INFO } from "../theme/blocks";

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

export interface AppState {
  /** False until the first getUiState() resolves — render skeletons before that. */
  ready: boolean;

  /* mirrored plugin state */
  chainOrder: BlockId[];
  model: ModelInfo | null;
  ir: FileEntry | null;
  models: FileEntry[];
  irs: FileEntry[];
  presets: PresetInfo[];
  currentPresetName: string;
  ab: AbState;
  t3k: T3kState;

  /* UI-local state */
  selected: BlockId | null;
  tone: T3kToneResult | null;
  /** modelId → progress in [0,1] while a TONE3000 download is in flight. */
  downloads: Record<number, number>;
  toasts: Toast[];
}

export interface AppActions {
  hydrate(): Promise<void>;

  /* chain */
  setChainOrder(order: BlockId[]): void;
  addBlock(id: BlockId): void;
  removeBlock(id: BlockId): void;
  selectBlock(id: BlockId | null): void;

  /* library / presets — thin wrappers that surface errors as toasts */
  loadModel(path: string): Promise<void>;
  clearModel(): Promise<void>;
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

  /* TONE3000 */
  t3kConfigure(): Promise<void>;
  t3kSignOut(): Promise<void>;
  t3kStartSelectFlow(): Promise<void>;
  t3kDownloadModel(model: T3kModel): Promise<void>;
  dismissTone(): void;

  /* toasts */
  toast(message: string, kind?: ToastKind, duration?: number): number;
  dismissToast(id: number): void;
}

export type Store = AppState & AppActions;

/** amp → first block → none (`ensureValidSelection`, preserved exactly). */
export function fallbackSelection(
  order: readonly BlockId[],
  current: BlockId | null,
): BlockId | null {
  if (current && order.includes(current)) return current;
  if (order.includes("amp")) return "amp";
  return order[0] ?? null;
}

function toBlockIds(tokens: string[]): BlockId[] {
  return tokens.filter(isBlockId);
}

let nextToastId = 1;

const initialState: AppState = {
  ready: false,
  chainOrder: [],
  model: null,
  ir: null,
  models: [],
  irs: [],
  presets: [],
  currentPresetName: "",
  ab: { activeSlot: 0, aHasState: false, bHasState: false },
  t3k: { configured: false, authenticated: false },
  selected: null,
  tone: null,
  downloads: {},
  toasts: [],
};

export const useStore = create<Store>()((set, get) => {
  /** Publish chain order to C++ — the ONLY place that writes it. */
  const publish = (order: BlockId[]): void => {
    void bridge.setChainOrder(order);
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
        model: state.model,
        ir: state.ir,
        models: state.models,
        irs: state.irs,
        presets: state.presets,
        currentPresetName: state.currentPresetName,
        ab: state.ab,
        t3k: state.t3k,
        selected: fallbackSelection(order, get().selected),
      });
    },

    /* ─────────────────────────────── chain ───────────────────────────── */

    setChainOrder(order) {
      set({
        chainOrder: order,
        selected: fallbackSelection(order, get().selected),
      });
      publish(order);
    },

    addBlock(id) {
      const { chainOrder } = get();
      if (chainOrder.includes(id)) {
        set({ selected: id });
        return;
      }
      const order = [...chainOrder, id];
      // Appending always selects the new block — it deliberately bypasses the
      // amp-priority fallback rule.
      set({ chainOrder: order, selected: id });
      publish(order);
    },

    removeBlock(id) {
      const order = get().chainOrder.filter((b) => b !== id);
      set({ chainOrder: order, selected: fallbackSelection(order, null) });
      publish(order);
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

    async t3kDownloadModel(model) {
      set((s) => ({ downloads: { ...s.downloads, [model.id]: 0 } }));
      await bridge.t3kDownloadModel(model);
    },

    dismissTone() {
      set({ tone: null });
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
    bridge.on("chainChanged", ({ chainOrder }) => {
      const order = toBlockIds(chainOrder);
      set({
        chainOrder: order,
        selected: fallbackSelection(order, get().selected),
      });
    }),

    bridge.on("libraryChanged", ({ models, irs }) => set({ models, irs })),

    bridge.on("presetChanged", ({ presets, currentPresetName, ab }) =>
      set({ presets, currentPresetName, ab }),
    ),

    bridge.on("modelChanged", ({ model }) => set({ model })),

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
      set((s) => {
        const downloads = { ...s.downloads };
        delete downloads[modelId];
        return { downloads };
      });
      // C++ only downloads + refreshes the library; loading is the UI's call.
      void get().loadModel(path);
      set({ tone: null });
    }),

    bridge.on("t3kError", ({ message, modelId }) => {
      // A download that failed never sends t3kComplete, so its progress row has
      // to be cleared here or it stays on the tone picker forever.
      if (modelId !== undefined) {
        set((s) => {
          const downloads = { ...s.downloads };
          delete downloads[modelId];
          return { downloads };
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

/** Blocks not currently in the chain, in the frozen enum order (add-menu order). */
export function selectAbsentBlocks(s: Store): BlockId[] {
  return (Object.keys(BLOCK_INFO) as BlockId[]).filter(
    (id) => !s.chainOrder.includes(id),
  );
}
