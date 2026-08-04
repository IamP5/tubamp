/**
 * Real bridge implementation over the vendored JUCE frontend lib.
 * Selected by ./index.ts whenever `window.__JUCE__` exists.
 */
import * as Juce from "../juce/index.js";
import type { JuceEventToken } from "../juce/global";
import type {
  AbSlot,
  Bridge,
  BridgeEventMap,
  BridgeEventName,
  ComboParamId,
  ComboParamState,
  EditorSize,
  EditorSizeLimits,
  ErrorResult,
  FxPluginEntry,
  FxSlotIndex,
  ImportResult,
  SliderParamId,
  SliderParamState,
  T3kBrowseRequest,
  T3kBrowseResult,
  T3kModel,
  T3kModelsResult,
  ToggleParamId,
  ToggleParamState,
  UiState,
  Unsubscribe,
} from "./types";

/* ─────────────────────────── native function plumbing ──────────────────── */

const nativeFns = new Map<string, (...args: unknown[]) => Promise<unknown>>();

function native(name: string): (...args: unknown[]) => Promise<unknown> {
  let fn = nativeFns.get(name);
  if (!fn) {
    fn = Juce.getNativeFunction(name);
    nativeFns.set(name, fn);
  }
  return fn;
}

/**
 * C++ sends results as JSON. Depending on how the completion packs them they
 * arrive either already parsed (a `var` round-tripped through the event bus) or
 * as a JSON string — accept both rather than guessing.
 */
function parse<T>(raw: unknown, fallback: T): T {
  if (raw === undefined || raw === null) return fallback;
  if (typeof raw === "string") {
    try {
      return JSON.parse(raw) as T;
    } catch {
      return fallback;
    }
  }
  return raw as T;
}

async function call<T>(name: string, fallback: T, ...args: unknown[]) {
  return parse<T>(await native(name)(...args), fallback);
}

/* ───────────────────────────────── params ─────────────────────────────── */

function listen(
  list: { addListener(fn: () => void): number; removeListener(id: number): void },
  fn: () => void,
): Unsubscribe {
  const id = list.addListener(fn);
  return () => list.removeListener(id);
}

function makeSliderState(id: SliderParamId): SliderParamState {
  const state = Juce.getSliderState(id);
  return {
    get properties() {
      return state.properties;
    },
    getNormalisedValue: () => state.getNormalisedValue(),
    getScaledValue: () => state.getScaledValue(),
    setNormalisedValue: (v) => state.setNormalisedValue(v),
    beginGesture: () => state.sliderDragStarted(),
    endGesture: () => state.sliderDragEnded(),
    onValueChanged: (fn) => listen(state.valueChangedEvent, fn),
    onPropertiesChanged: (fn) => listen(state.propertiesChangedEvent, fn),
  };
}

function makeToggleState(id: ToggleParamId): ToggleParamState {
  const state = Juce.getToggleState(id);
  return {
    get properties() {
      return state.properties;
    },
    getValue: () => state.getValue(),
    setValue: (v) => state.setValue(v),
    onValueChanged: (fn) => listen(state.valueChangedEvent, fn),
    onPropertiesChanged: (fn) => listen(state.propertiesChangedEvent, fn),
  };
}

function makeComboState(id: ComboParamId): ComboParamState {
  const state = Juce.getComboBoxState(id);
  return {
    get properties() {
      return state.properties;
    },
    getChoiceIndex: () => state.getChoiceIndex(),
    setChoiceIndex: (i) => state.setChoiceIndex(i),
    onValueChanged: (fn) => listen(state.valueChangedEvent, fn),
    onPropertiesChanged: (fn) => listen(state.propertiesChangedEvent, fn),
  };
}

/* Param states are singletons per id — JUCE keeps one state object per relay,
   and hooks in different components must observe the same instance. */
const sliderStates = new Map<string, SliderParamState>();
const toggleStates = new Map<string, ToggleParamState>();
const comboStates = new Map<string, ComboParamState>();

/* ────────────────────────────────── bridge ─────────────────────────────── */

/**
 * Only ever used when a native call fails outright. It has to be a size the page
 * can lay out in rather than zeroes, because the grip clamps against it until
 * the first `editorSizeChanged` lands — hence the minimum from
 * theme/tokens.css, and a maximum that refuses to grow rather than one that
 * invents a display size.
 */
const fallbackEditorSize: EditorSizeLimits = {
  width: 1280,
  height: 800,
  minWidth: 1000,
  minHeight: 600,
  maxWidth: 1280,
  maxHeight: 800,
};

const emptyUiState: UiState = {
  chainOrder: [],
  chainRows: [],
  model: null,
  ir: null,
  models: [],
  irs: [],
  presets: [],
  currentPresetName: "",
  ab: { activeSlot: 0, aHasState: false, bHasState: false },
  t3k: { configured: false, authenticated: false, username: null },
  fxSlots: [],
  editorSize: fallbackEditorSize,
  fxEmbed: { slot: -1, width: 0, height: 0, error: "" },
  fxSupported: false,
};

export const juceBridge: Bridge = {
  kind: "juce",

  paramIndexUpdater: (attribute) =>
    new Juce.ControlParameterIndexUpdater(attribute),

  sliderState(id) {
    let s = sliderStates.get(id);
    if (!s) {
      s = makeSliderState(id);
      sliderStates.set(id, s);
    }
    return s;
  },

  toggleState(id) {
    let s = toggleStates.get(id);
    if (!s) {
      s = makeToggleState(id);
      toggleStates.set(id, s);
    }
    return s;
  },

  comboState(id) {
    let s = comboStates.get(id);
    if (!s) {
      s = makeComboState(id);
      comboStates.set(id, s);
    }
    return s;
  },

  on<E extends BridgeEventName>(
    event: E,
    listener: (payload: BridgeEventMap[E]) => void,
  ): Unsubscribe {
    const backend = window.__JUCE__?.backend;
    if (!backend) return () => {};
    const token: JuceEventToken = backend.addEventListener(event, listener);
    return () => backend.removeEventListener(token);
  },

  getUiState: () => call<UiState>("getUiState", emptyUiState),
  setChainOrder: async (tokens, rows) => {
    await native("setChainOrder")(tokens, rows);
  },
  loadModel: (path) => call<ErrorResult>("loadModel", {}, path),
  clearModel: async () => {
    await native("clearModel")();
  },
  loadIr: (path) => call<ErrorResult>("loadIr", {}, path),
  clearIr: async () => {
    await native("clearIr")();
  },
  importModel: () => call<ImportResult>("importModel", {}),
  importIr: () => call<ImportResult>("importIr", {}),
  loadPreset: (path) => call<ErrorResult>("loadPreset", {}, path),
  savePresetAs: () => call<ErrorResult>("savePresetAs", {}),
  deletePreset: (path) => call<ErrorResult>("deletePreset", {}, path),
  setPresetFavorite: async (path, favorite) => {
    await native("setPresetFavorite")(path, favorite);
  },
  abCapture: async (slot: AbSlot) => {
    await native("abCapture")(slot);
  },
  abRecall: async (slot: AbSlot) => {
    await native("abRecall")(slot);
  },
  t3kConfigure: () => call<ErrorResult>("t3kConfigure", {}),
  t3kSignOut: async () => {
    await native("t3kSignOut")();
  },
  t3kStartSelectFlow: async () => {
    await native("t3kStartSelectFlow")();
  },
  t3kDownloadModel: async (model: T3kModel) => {
    await native("t3kDownloadModel")(model);
  },
  t3kSignIn: async () => {
    await native("t3kSignIn")();
  },
  // A failed page must still resolve: the drawer renders `{error}` as a retry
  // state, and a rejected promise would leave it stuck on the spinner.
  t3kBrowse: (request: T3kBrowseRequest) =>
    call<T3kBrowseResult>(
      "t3kBrowse",
      { error: "TONE3000 is not reachable." },
      request,
    ),
  t3kListModels: (toneId) =>
    call<T3kModelsResult>(
      "t3kListModels",
      { error: "Could not list this tone's files." },
      toneId,
    ),
  t3kSetFavorite: (toneId, favorite) =>
    call<ErrorResult>("t3kSetFavorite", {}, toneId, favorite),

  /* --- external AudioUnit slots ------------------------------------------- */

  // A build without plugin hosting still answers, with `supported: false` — the
  // panel explains itself rather than offering an empty picker, so the fallback
  // must not look like "no plugins installed".
  fxListPlugins: () =>
    call<{ plugins: FxPluginEntry[]; supported: boolean }>("fxListPlugins", {
      plugins: [],
      supported: false,
    }),
  fxLoad: (slot: FxSlotIndex, identifier) =>
    call<ErrorResult>("fxLoad", {}, slot, identifier),
  fxClear: async (slot: FxSlotIndex) => {
    await native("fxClear")(slot);
  },
  fxOpenEditor: (slot: FxSlotIndex) =>
    call<ErrorResult>("fxOpenEditor", {}, slot),
  fxSetParam: async (slot: FxSlotIndex, index, value) => {
    await native("fxSetParam")(slot, index, value);
  },
  fxBeginGesture: async (slot: FxSlotIndex, index) => {
    await native("fxBeginGesture")(slot, index);
  },
  fxEndGesture: async (slot: FxSlotIndex, index) => {
    await native("fxEndGesture")(slot, index);
  },
  fxWatchSlot: async (slot) => {
    await native("fxWatchSlot")(slot);
  },

  /* --- window size --------------------------------------------------------- */

  getEditorSize: () => call<EditorSizeLimits>("getEditorSize", fallbackEditorSize),
  // Resolves with what C++ actually applied after clamping, which is not
  // necessarily what was asked for; the authoritative broadcast is the
  // `editorSizeChanged` event, which fires for this call too.
  setEditorSize: (width, height) =>
    call<EditorSize>("setEditorSize", { width, height }, width, height),

  /* --- embedding a hosted plugin's own editor ------------------------------ */

  fxSetEmbedSlot: (slot) => call<ErrorResult>("fxSetEmbedSlot", {}, slot),
  fxSetEmbedRect: async (x, y, width, height) => {
    await native("fxSetEmbedRect")(x, y, width, height);
  },
  fxSetEmbedMinWindow: async (width, height) => {
    await native("fxSetEmbedMinWindow")(width, height);
  },

  fxSetEmbedVisible: async (visible) => {
    await native("fxSetEmbedVisible")(visible);
  },
};

/* The two new events (`editorSizeChanged`, `fxEmbedChanged`) need no wiring of
   their own: `on()` above forwards any name in `BridgeEventMap` straight to the
   JUCE backend's event bus, and C++ emits them under those exact names. */
