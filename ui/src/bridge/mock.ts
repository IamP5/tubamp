/**
 * Mock bridge — used whenever `window.__JUCE__` is absent, i.e. `npm run dev`
 * in a normal browser. It is the only way to iterate on the UI now that the
 * headless screenshot target is gone, so it must behave like the real plugin:
 * real parameter ranges/skews/defaults (docs/research/current-ui-inventory.md
 * §Parameters), 30 Hz meters, async T3K flows, chain persistence.
 *
 * Everything here is in-memory only — no web storage (the real origin is
 * `juce://juce.backend`, where persistence is C++'s job).
 */
import type {
  AbSlot,
  Bridge,
  BridgeEventMap,
  BridgeEventName,
  ComboParamId,
  ComboParamState,
  ComboProperties,
  ErrorResult,
  FileEntry,
  ImportResult,
  ModelInfo,
  PresetInfo,
  SliderParamId,
  SliderParamState,
  SliderProperties,
  T3kModel,
  ToggleParamId,
  ToggleParamState,
  ToggleProperties,
  UiState,
  Unsubscribe,
} from "./types";
import {
  COMBO_SPECS,
  PARAM_INDEX,
  SLIDER_SPECS,
  TOGGLE_SPECS,
  skewFor,
  type SliderSpec,
} from "./paramMeta";

/* ───────────────────────────── event dispatcher ────────────────────────── */

type Listener = (payload: never) => void;
const listeners = new Map<string, Set<Listener>>();

function emit<E extends BridgeEventName>(
  event: E,
  payload: BridgeEventMap[E],
): void {
  const set = listeners.get(event);
  if (!set) return;
  for (const fn of [...set]) (fn as (p: BridgeEventMap[E]) => void)(payload);
}

/* ───────────────────────────── parameter model ─────────────────────────── */

class ListenerSet {
  private readonly fns = new Set<() => void>();
  add(fn: () => void): Unsubscribe {
    this.fns.add(fn);
    return () => this.fns.delete(fn);
  }
  fire(): void {
    for (const fn of [...this.fns]) fn();
  }
}

class MockSlider implements SliderParamState {
  readonly properties: SliderProperties;
  private scaled: number;
  private readonly valueListeners = new ListenerSet();
  private readonly propListeners = new ListenerSet();

  constructor(spec: SliderSpec) {
    this.properties = {
      start: spec.min,
      end: spec.max,
      skew: skewFor(spec),
      name: spec.name,
      label: spec.label,
      numSteps: Math.round((spec.max - spec.min) / spec.interval) + 1,
      interval: spec.interval,
      parameterIndex: PARAM_INDEX[spec.id] ?? -1,
    };
    this.scaled = spec.def;
  }

  getScaledValue(): number {
    return this.scaled;
  }

  getNormalisedValue(): number {
    const { start, end, skew } = this.properties;
    return Math.pow((this.scaled - start) / (end - start), skew);
  }

  setNormalisedValue(value: number): void {
    const { start, end, skew, interval } = this.properties;
    const clamped = Math.min(1, Math.max(0, value));
    const raw = Math.pow(clamped, 1 / skew) * (end - start) + start;
    const snapped =
      interval === 0
        ? raw
        : Math.min(
            end,
            Math.max(start, start + interval * Math.floor((raw - start) / interval + 0.5)),
          );
    if (snapped === this.scaled) return;
    this.scaled = snapped;
    this.valueListeners.fire();
  }

  beginGesture(): void {}
  endGesture(): void {}

  onValueChanged(fn: () => void): Unsubscribe {
    return this.valueListeners.add(fn);
  }
  onPropertiesChanged(fn: () => void): Unsubscribe {
    return this.propListeners.add(fn);
  }

  /** Mock-only: simulate an external (host/preset) value change. */
  setScaledExternally(value: number): void {
    this.scaled = value;
    this.valueListeners.fire();
  }
}

class MockToggle implements ToggleParamState {
  readonly properties: ToggleProperties;
  private value: boolean;
  private readonly valueListeners = new ListenerSet();
  private readonly propListeners = new ListenerSet();

  constructor(spec: { id: ToggleParamId; name: string; def: boolean }) {
    this.properties = {
      name: spec.name,
      parameterIndex: PARAM_INDEX[spec.id] ?? -1,
    };
    this.value = spec.def;
  }

  getValue(): boolean {
    return this.value;
  }
  setValue(value: boolean): void {
    if (value === this.value) return;
    this.value = value;
    this.valueListeners.fire();
  }
  onValueChanged(fn: () => void): Unsubscribe {
    return this.valueListeners.add(fn);
  }
  onPropertiesChanged(fn: () => void): Unsubscribe {
    return this.propListeners.add(fn);
  }
}

class MockCombo implements ComboParamState {
  readonly properties: ComboProperties;
  private index: number;
  private readonly valueListeners = new ListenerSet();
  private readonly propListeners = new ListenerSet();

  constructor(spec: {
    id: ComboParamId;
    name: string;
    choices: string[];
    def: number;
  }) {
    this.properties = {
      name: spec.name,
      parameterIndex: PARAM_INDEX[spec.id] ?? -1,
      choices: spec.choices,
    };
    this.index = spec.def;
  }

  getChoiceIndex(): number {
    return this.index;
  }
  setChoiceIndex(index: number): void {
    if (index === this.index) return;
    this.index = index;
    this.valueListeners.fire();
  }
  onValueChanged(fn: () => void): Unsubscribe {
    return this.valueListeners.add(fn);
  }
  onPropertiesChanged(fn: () => void): Unsubscribe {
    return this.propListeners.add(fn);
  }
}

const sliders = new Map<SliderParamId, MockSlider>(
  SLIDER_SPECS.map((s) => [s.id, new MockSlider(s)]),
);
const toggles = new Map<ToggleParamId, MockToggle>(
  TOGGLE_SPECS.map((s) => [s.id, new MockToggle(s)]),
);
const combos = new Map<ComboParamId, MockCombo>(
  COMBO_SPECS.map((s) => [s.id, new MockCombo(s)]),
);

/* ────────────────────────────── mock plugin state ──────────────────────── */

const MODELS_DIR = "/Users/dev/Music/tubamp/models";
const IRS_DIR = "/Users/dev/Music/tubamp/irs";
const PRESETS_DIR = "/Users/dev/Music/tubamp/presets";

const loadedModel: ModelInfo = {
  path: `${MODELS_DIR}/Marshall JCM800 Crunch.nam`,
  name: "Marshall JCM800 Crunch",
  sampleRateHz: 48000,
  loudnessDb: -13.4,
  inputLevelDbu: 12.5,
  outputLevelDbu: 11.2,
  isSlimmable: true,
  latencySamples: 90,
};

const state: UiState = {
  chainOrder: ["gate", "comp", "drive", "amp", "cab", "eq", "mod", "delay", "reverb"],
  model: loadedModel,
  ir: { path: `${IRS_DIR}/Greenback 4x12 SM57 Cap.wav`, name: "Greenback 4x12 SM57 Cap" },
  models: [
    { path: `${MODELS_DIR}/Marshall JCM800 Crunch.nam`, name: "Marshall JCM800 Crunch" },
    { path: `${MODELS_DIR}/Fender Twin Clean.nam`, name: "Fender Twin Clean" },
    { path: `${MODELS_DIR}/Mesa Rectifier Modern.nam`, name: "Mesa Rectifier Modern" },
    { path: `${MODELS_DIR}/Vox AC30 Top Boost.nam`, name: "Vox AC30 Top Boost" },
    { path: `${MODELS_DIR}/Dumble ODS Clean.nam`, name: "Dumble ODS Clean" },
  ],
  irs: [
    { path: `${IRS_DIR}/Greenback 4x12 SM57 Cap.wav`, name: "Greenback 4x12 SM57 Cap" },
    { path: `${IRS_DIR}/V30 4x12 R121 Edge.wav`, name: "V30 4x12 R121 Edge" },
    { path: `${IRS_DIR}/Jensen 1x12 Ribbon.wav`, name: "Jensen 1x12 Ribbon" },
  ],
  presets: [
    { name: "Init", path: `${PRESETS_DIR}/Init.tubamp`, favorite: false, tags: [] },
    { name: "Crunch Rhythm", path: `${PRESETS_DIR}/Crunch Rhythm.tubamp`, favorite: true, tags: ["rock", "rhythm"] },
    { name: "Ambient Lead", path: `${PRESETS_DIR}/Ambient Lead.tubamp`, favorite: true, tags: ["lead", "wet"] },
    { name: "Clean Chorus", path: `${PRESETS_DIR}/Clean Chorus.tubamp`, favorite: false, tags: ["clean"] },
    { name: "Bedroom Metal", path: `${PRESETS_DIR}/Bedroom Metal.tubamp`, favorite: false, tags: ["metal"] },
    { name: "Slapback Country", path: `${PRESETS_DIR}/Slapback Country.tubamp`, favorite: false, tags: ["clean", "delay"] },
  ],
  currentPresetName: "Crunch Rhythm",
  ab: { activeSlot: 0, aHasState: true, bHasState: false },
  t3k: { configured: true, authenticated: true },
};

function snapshot(): UiState {
  return structuredClone(state);
}

/* ──────────────────────────────── fake meters ──────────────────────────── */

/**
 * One rAF driver emitting `meters` at ~30 Hz, like the C++ editor timer.
 * The envelope is deliberately musical: note attacks every ~600-900 ms with an
 * exponential decay, a slow strum-level drift and a little noise, so meter
 * smoothing/peak-hold can actually be judged by eye.
 */
let meterRaf = 0;
let lastMeterEmit = 0;
let noteStart = 0;
let notePeak = 0.6;
let nextNoteIn = 400;

function meterTick(now: number): void {
  meterRaf = requestAnimationFrame(meterTick);
  if (now - lastMeterEmit < 1000 / 30) return;
  lastMeterEmit = now;

  if (now - noteStart > nextNoteIn) {
    noteStart = now;
    nextNoteIn = 420 + Math.random() * 620;
    notePeak = 0.35 + Math.random() * 0.6;
  }

  const age = (now - noteStart) / 1000;
  const body = notePeak * Math.exp(-age * 1.6);
  const shimmer = 1 + 0.06 * Math.sin(now / 90);
  const noise = 0.01 * Math.random();
  const input = Math.min(1, body * shimmer + noise);
  // Output runs a touch hotter and decays slower — amp + reverb tail.
  const output = Math.min(1, input * 1.15 + notePeak * 0.18 * Math.exp(-age * 0.8));

  emit("meters", { in: input, out: output });
}

function startMeters(): void {
  if (meterRaf || typeof requestAnimationFrame !== "function") return;
  meterRaf = requestAnimationFrame(meterTick);
}

/* ─────────────────────────────── T3K simulation ────────────────────────── */

const T3K_TONES: { toneId: number; models: T3kModel[] }[] = [
  {
    toneId: 41207,
    models: [
      { id: 90211, name: "JCM800 2203 — Crunch (Standard)", modelUrl: "https://tone3000.test/m/90211.nam", size: "Standard", architecture: "WaveNet" },
      { id: 90212, name: "JCM800 2203 — Crunch (Lite)", modelUrl: "https://tone3000.test/m/90212.nam", size: "Lite", architecture: "WaveNet" },
      { id: 90213, name: "JCM800 2203 — Crunch (Feather)", modelUrl: "https://tone3000.test/m/90213.nam", size: "Feather", architecture: "LSTM" },
    ],
  },
  {
    toneId: 55810,
    models: [
      { id: 77104, name: "AC30 Top Boost — Edge (Standard)", modelUrl: "https://tone3000.test/m/77104.nam", size: "Standard", architecture: "WaveNet" },
      { id: 77105, name: "AC30 Top Boost — Edge (Nano)", modelUrl: "https://tone3000.test/m/77105.nam", size: "Nano", architecture: "LSTM" },
    ],
  },
  /* A zero-model tone: the inventory doc flags this as a silent failure today —
     the React UI must surface a toast instead. */
  { toneId: 60002, models: [] },
];

let nextTone = 0;
const downloads = new Map<number, number>();

/** `?t3kfail` makes every simulated download die half-way, so the failure path
 *  (t3kError carrying a modelId, progress row cleared) is reachable in dev. */
const failDownloads = new URLSearchParams(window.location.search).has("t3kfail");

function simulateDownload(model: T3kModel): void {
  if (downloads.has(model.id)) return;
  let progress = 0;
  const step = (): void => {
    progress = Math.min(1, progress + 0.03 + Math.random() * 0.05);
    if (failDownloads && progress > 0.4) {
      downloads.delete(model.id);
      emit("t3kError", {
        message: "Could not download the model from TONE3000 (HTTP 503).",
        modelId: model.id,
      });
      return;
    }
    emit("t3kProgress", { modelId: model.id, progress });
    if (progress < 1) {
      downloads.set(model.id, window.setTimeout(step, 90));
      return;
    }
    downloads.delete(model.id);
    const entry: FileEntry = {
      path: `${MODELS_DIR}/${model.name}.nam`,
      name: model.name,
    };
    if (!state.models.some((m) => m.path === entry.path)) {
      state.models = [...state.models, entry];
      emit("libraryChanged", { models: [...state.models], irs: [...state.irs] });
    }
    emit("t3kComplete", { modelId: model.id, path: entry.path });
  };
  downloads.set(model.id, window.setTimeout(step, 120));
}

/* ────────────────────────────────── bridge ─────────────────────────────── */

function delay<T>(value: T, ms = 140): Promise<T> {
  return new Promise((resolve) => window.setTimeout(() => resolve(value), ms));
}

function emitLibrary(): void {
  emit("libraryChanged", { models: [...state.models], irs: [...state.irs] });
}

function emitPresets(): void {
  emit("presetChanged", {
    presets: structuredClone(state.presets),
    currentPresetName: state.currentPresetName,
    ab: { ...state.ab },
  });
}

let importCounter = 0;

export const mockBridge: Bridge = {
  kind: "mock",

  /* No host to tell, so this is a no-op outside the plugin. */
  paramIndexUpdater: () => ({ handleMouseMove: () => {} }),

  sliderState(id: SliderParamId) {
    const s = sliders.get(id);
    if (!s) throw new Error(`mock bridge: unknown slider param "${id}"`);
    return s;
  },
  toggleState(id: ToggleParamId) {
    const t = toggles.get(id);
    if (!t) throw new Error(`mock bridge: unknown toggle param "${id}"`);
    return t;
  },
  comboState(id: ComboParamId) {
    const c = combos.get(id);
    if (!c) throw new Error(`mock bridge: unknown combo param "${id}"`);
    return c;
  },

  on<E extends BridgeEventName>(
    event: E,
    listener: (payload: BridgeEventMap[E]) => void,
  ): Unsubscribe {
    let set = listeners.get(event);
    if (!set) {
      set = new Set();
      listeners.set(event, set);
    }
    const fn = listener as Listener;
    set.add(fn);
    if (event === "meters") startMeters();
    return () => {
      set.delete(fn);
    };
  },

  getUiState: () => delay(snapshot(), 60),

  setChainOrder: async (tokens) => {
    state.chainOrder = [...tokens];
    await delay(null, 0);
    emit("chainChanged", { chainOrder: [...state.chainOrder] });
  },

  loadModel: async (path) => {
    const entry = state.models.find((m) => m.path === path);
    if (!entry) return delay<ErrorResult>({ error: "Model file not found" });
    state.model = {
      ...loadedModel,
      path: entry.path,
      name: entry.name,
      isSlimmable: !entry.name.toLowerCase().includes("lite"),
      latencySamples: 90,
    };
    emit("modelChanged", { model: structuredClone(state.model) });
    return delay<ErrorResult>({}, 220);
  },

  clearModel: async () => {
    state.model = null;
    await delay(null, 0);
    emit("modelChanged", { model: null });
  },

  loadIr: async (path) => {
    const entry = state.irs.find((i) => i.path === path);
    if (!entry) return delay<ErrorResult>({ error: "IR file not found" });
    state.ir = { ...entry };
    emit("irChanged", { ir: { ...entry } });
    return delay<ErrorResult>({}, 120);
  },

  clearIr: async () => {
    state.ir = null;
    await delay(null, 0);
    emit("irChanged", { ir: null });
  },

  // Import installs the file *and* loads it, like WebEditor's native functions —
  // the UI must not load again on the returned path.
  importModel: async () => {
    importCounter += 1;
    const entry: FileEntry = {
      path: `${MODELS_DIR}/Imported Model ${importCounter}.nam`,
      name: `Imported Model ${importCounter}`,
    };
    state.models = [...state.models, entry];
    await delay(null, 400); // native file chooser round-trip
    emitLibrary();
    state.model = {
      ...loadedModel,
      path: entry.path,
      name: entry.name,
      isSlimmable: true,
      latencySamples: 90,
    };
    emit("modelChanged", { model: structuredClone(state.model) });
    return { path: entry.path, name: entry.name } satisfies ImportResult;
  },

  importIr: async () => {
    importCounter += 1;
    const entry: FileEntry = {
      path: `${IRS_DIR}/Imported IR ${importCounter}.wav`,
      name: `Imported IR ${importCounter}`,
    };
    state.irs = [...state.irs, entry];
    await delay(null, 400);
    emitLibrary();
    state.ir = { ...entry };
    emit("irChanged", { ir: { ...entry } });
    return { path: entry.path, name: entry.name } satisfies ImportResult;
  },

  loadPreset: async (path) => {
    const preset = state.presets.find((p) => p.path === path);
    if (!preset) return delay<ErrorResult>({ error: "Preset not found" });
    state.currentPresetName = preset.name;
    // A preset load restores chain order + param values in the real plugin.
    for (const spec of SLIDER_SPECS) {
      const jitter = spec.log ? 1 : 0;
      const span = (spec.max - spec.min) * 0.05 * Math.random() * jitter;
      sliders.get(spec.id)?.setScaledExternally(
        Math.min(spec.max, Math.max(spec.min, spec.def + span)),
      );
    }
    emitPresets();
    emit("chainChanged", { chainOrder: [...state.chainOrder] });
    return delay<ErrorResult>({}, 180);
  },

  savePresetAs: async () => {
    await delay(null, 500); // native AlertWindow text prompt
    const name = `User Preset ${state.presets.length - 5 || 1}`;
    const preset: PresetInfo = {
      name,
      path: `${PRESETS_DIR}/${name}.tubamp`,
      favorite: false,
      tags: [],
    };
    state.presets = [...state.presets, preset];
    state.currentPresetName = name;
    emitPresets();
    return {} satisfies ErrorResult;
  },

  deletePreset: async (path) => {
    const preset = state.presets.find((p) => p.path === path);
    if (!preset) return delay<ErrorResult>({ error: "Preset not found" });
    state.presets = state.presets.filter((p) => p.path !== path);
    if (state.currentPresetName === preset.name) state.currentPresetName = "";
    emitPresets();
    return delay<ErrorResult>({}, 100);
  },

  setPresetFavorite: async (path, favorite) => {
    state.presets = state.presets.map((p) =>
      p.path === path ? { ...p, favorite } : p,
    );
    await delay(null, 0);
    emitPresets();
  },

  abCapture: async (slot: AbSlot) => {
    state.ab = {
      ...state.ab,
      ...(slot === 0 ? { aHasState: true } : { bHasState: true }),
    };
    await delay(null, 0);
    emitPresets();
  },

  abRecall: async (slot: AbSlot) => {
    state.ab = { ...state.ab, activeSlot: slot };
    await delay(null, 0);
    emitPresets();
  },

  t3kConfigure: async () => {
    await delay(null, 600); // native AlertWindow key prompt
    state.t3k = { configured: true, authenticated: true };
    emit("t3kStatus", { ...state.t3k });
    return {} satisfies ErrorResult;
  },

  t3kSignOut: async () => {
    state.t3k = { configured: false, authenticated: false };
    await delay(null, 0);
    emit("t3kStatus", { ...state.t3k });
  },

  t3kStartSelectFlow: async () => {
    if (!state.t3k.configured) {
      await delay(null, 200);
      emit("t3kError", { message: "Add your TONE3000 key in Settings first." });
      return;
    }
    await delay(null, 900); // browser round-trip + user picking a tone
    const tone = T3K_TONES[nextTone % T3K_TONES.length]!;
    nextTone += 1;
    emit("t3kToneSelected", { toneId: tone.toneId, models: [...tone.models] });
  },

  t3kDownloadModel: async (model: T3kModel) => {
    await delay(null, 0);
    simulateDownload(model);
  },
};
