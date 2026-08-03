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
  EditorSize,
  EditorSizeLimits,
  ErrorResult,
  FileEntry,
  FxEmbedState,
  FxParamInfo,
  FxPluginEntry,
  FxSlotIndex,
  FxSlotState,
  ImportResult,
  ModelInfo,
  PresetInfo,
  SliderParamId,
  SliderParamState,
  SliderProperties,
  T3kBrowseRequest,
  T3kBrowseResult,
  T3kModel,
  T3kModelsResult,
  T3kTone,
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

/* ─────────────────────── external AudioUnit simulation ─────────────────── */

/**
 * A fake AU catalog for the three fx slots. It exists to make every state the
 * real host can produce reachable in the browser, so it deliberately includes
 * the awkward cases: a name far longer than any column, a two-character name,
 * several manufacturers, a plugin with sixty parameters, one that only speaks in
 * steps, one that reports unit labels, and one that always fails to instantiate.
 */
interface MockFxParam {
  name: string;
  /** Unit suffix the plugin reports; often empty, exactly like the real thing. */
  label: string;
  /** Normalised default. */
  def: number;
  /** The plugin's own formatting of a normalised value. */
  format(value: number): string;
}

/**
 * What happens when this plugin's editor is asked to draw inside our window.
 * Three behaviours worth having on hand, because each drives a different path
 * through features/embed and none of them is reachable from the others.
 */
interface MockFxEmbed {
  /** The editor's real size, once its view has attached. */
  width: number;
  height: number;
  /** A first, wrong size reported before the view attaches — several real AUs
   *  do this, and it is the only way to exercise the re-layout path. */
  provisional?: { width: number; height: number };
  /** Non-empty: embedding is refused and the caller must fall back to a window. */
  refuse?: string;
}

interface MockFxPlugin {
  identifier: string;
  name: string;
  manufacturer: string;
  version: string;
  hasEditor: boolean;
  /** Installed, listed, and dies on instantiation — the third failure mode after
   *  "not installed" (missing) and "loads fine". */
  failsToLoad?: boolean;
  latencySamples: number;
  embed: MockFxEmbed;
  params: MockFxParam[];
}

const pct = (v: number): string => `${Math.round(v * 100)} %`;
const dB = (min: number, max: number) => (v: number) =>
  `${(min + v * (max - min)).toFixed(1)} dB`;
/** Log-spaced, like every frequency control a real plugin exposes. */
const hz = (min: number, max: number) => (v: number) =>
  `${Math.round(min * Math.pow(max / min, v))} Hz`;
const step =
  (choices: readonly string[]) =>
  (v: number): string =>
    choices[Math.min(choices.length - 1, Math.floor(v * choices.length))]!;

function fxParam(
  name: string,
  def: number,
  label = "",
  format: (value: number) => string = pct,
): MockFxParam {
  return { name, label, def, format };
}

/** The 60-parameter case: a plugin whose list cannot fit the dock without
 *  scrolling, which is the whole point of testing against it. */
function bandParams(): MockFxParam[] {
  const params: MockFxParam[] = [];
  for (let band = 1; band <= 20; band += 1) {
    params.push(
      fxParam(`Band ${band} Gain`, 0.5, "dB", dB(-24, 24)),
      fxParam(`Band ${band} Freq`, 0.5, "Hz", hz(20, 20000)),
      fxParam(`Band ${band} Q`, 0.35, "", (v) => (0.1 + v * 9.9).toFixed(2)),
    );
  }
  return params;
}

const FX_PLUGINS: MockFxPlugin[] = [
  {
    identifier: "com.anodized.vc2a",
    name: "Valve Compressor VC-2A",
    manufacturer: "Anodized Audio",
    version: "2.1.0",
    hasEditor: true,
    latencySamples: 0,
    embed: { width: 520, height: 280 },
    params: [
      fxParam("Peak Reduction", 0.4),
      fxParam("Gain", 0.5),
      fxParam("Emphasis", 0.25),
      fxParam("Mix", 1),
    ],
  },
  {
    identifier: "com.northfold.spectral",
    name: "Spectral Convolution Workstation Mk III (Surround Edition)",
    manufacturer: "Northfold DSP",
    version: "3.0.4",
    hasEditor: true,
    latencySamples: 1024,
    // The refusal case: an editor bigger than any display we could grow to.
    embed: {
      width: 2400,
      height: 1400,
      refuse:
        "Spectral Convolution Workstation needs a 2400 x 1400 editor, which is larger than this display.",
    },
    params: bandParams(),
  },
  {
    identifier: "com.northfold.eq",
    name: "EQ",
    manufacturer: "Northfold DSP",
    version: "1.4.2",
    hasEditor: false,
    latencySamples: 0,
    // No editor of its own, so this is the host's generic one — still a native
    // view, still a hole in the page.
    embed: { width: 420, height: 200 },
    params: [
      fxParam("Low Shelf", 0.5, "dB", dB(-18, 18)),
      fxParam("Low Freq", 0.2, "Hz", hz(20, 1000)),
      fxParam("High Shelf", 0.5, "dB", dB(-18, 18)),
      fxParam("High Freq", 0.8, "Hz", hz(1000, 20000)),
      fxParam("Output", 0.5, "dB", dB(-12, 12)),
    ],
  },
  {
    identifier: "com.kilohertz.tape",
    name: "Studer-Style Tape",
    manufacturer: "Kilohertz Labs",
    version: "1.0.9",
    hasEditor: true,
    latencySamples: 64,
    // Reports twice, and the real size is taller than the board band at any
    // window this app opens at — so it also drives the "ask for a bigger
    // window" path.
    embed: {
      width: 880,
      height: 460,
      provisional: { width: 320, height: 180 },
    },
    params: [
      fxParam("Machine", 0, "", step(["A80", "A812", "B67", "M15"])),
      fxParam("Speed", 0.5, "ips", step(["7.5 ips", "15 ips", "30 ips"])),
      fxParam("Formula", 0.5, "", step(["456", "GP9", "SM911"])),
      fxParam("Bias", 0.5, "", (v) => `${(v * 10 - 5).toFixed(1)}`),
      fxParam("Drive", 0.35, "dB", dB(0, 24)),
      fxParam("Wow & Flutter", 0.2),
    ],
  },
  {
    identifier: "com.vermilion.panic",
    name: "Kernel Panic Reverb",
    manufacturer: "Vermilion Audio",
    version: "0.9.0-beta",
    hasEditor: true,
    failsToLoad: true,
    latencySamples: 0,
    embed: { width: 400, height: 300 },
    params: [],
  },
  {
    identifier: "com.t0audio.utility",
    name: "Utility",
    manufacturer: "t0audio",
    version: "1.0.0",
    hasEditor: false,
    latencySamples: 0,
    embed: { width: 360, height: 160 },
    params: [
      fxParam("Gain", 0.5, "dB", dB(-24, 24)),
      fxParam("Pan", 0.5, "", (v) => `${Math.round(v * 200 - 100)} L/R`),
      fxParam("Phase", 0, "", step(["Normal", "Inverted"])),
    ],
  },
];

/** Assigned to slot 3 and absent from the catalog: the "you opened this session
 *  on a machine without that plugin" case, whose settings must survive untouched. */
const MISSING_FX = {
  identifier: "com.obsidian.tapedelay",
  name: "Obsidian Tape Delay",
  manufacturer: "Obsidian Instruments",
};

function findFxPlugin(identifier: string): MockFxPlugin | undefined {
  return FX_PLUGINS.find((p) => p.identifier === identifier);
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

/** Normalised values per slot, index-aligned with the loaded plugin's params. */
const fxValues: number[][] = [[], [], []];
const fxLoaded: (MockFxPlugin | null)[] = [null, null, null];

function fxParamInfos(plugin: MockFxPlugin, slot: FxSlotIndex): FxParamInfo[] {
  return plugin.params.map((p, index) => {
    const value = fxValues[slot][index] ?? p.def;
    return { index, name: p.name, label: p.label, value, text: p.format(value) };
  });
}

function liveFxSlot(plugin: MockFxPlugin, slot: FxSlotIndex): FxSlotState {
  return {
    slot,
    identifier: plugin.identifier,
    name: plugin.name,
    manufacturer: plugin.manufacturer,
    occupied: true,
    missing: false,
    live: true,
    loading: false,
    latencySamples: plugin.latencySamples,
    error: "",
    hasEditor: plugin.hasEditor,
    params: fxParamInfos(plugin, slot),
  };
}

function emitFxSlot(slot: FxSlotIndex, next: FxSlotState): void {
  state.fxSlots[slot] = next;
  emit("fxSlotChanged", { slot, state: structuredClone(next) });
}

/* Slot 2 opens with a plugin already live and slot 3 with one that is missing,
   so the two states that are otherwise a chore to reach are on screen at once. */
function initialFxSlots(): FxSlotState[] {
  const preloaded = FX_PLUGINS[0]!;
  fxLoaded[1] = preloaded;
  fxValues[1] = preloaded.params.map((p) => p.def);
  return [
    emptyFxSlot(0),
    liveFxSlot(preloaded, 1),
    {
      ...emptyFxSlot(2),
      ...MISSING_FX,
      occupied: true,
      missing: true,
      error: `${MISSING_FX.name} is not installed on this machine.`,
    },
  ];
}

/**
 * Metered parameter push for the watched slot, on the same ~30 Hz rAF cadence as
 * the C++ editor timer. Parameter 0 is swept by a slow LFO on purpose: it stands
 * in for a plugin moving its own parameters, which is what makes the drag guard
 * in FxParamGrid worth having (and visibly broken if it is removed).
 */
let fxWatched: FxSlotIndex | -1 = -1;
let fxRaf = 0;
let lastFxEmit = 0;

function fxTick(now: number): void {
  fxRaf = requestAnimationFrame(fxTick);
  if (now - lastFxEmit < 1000 / 30) return;
  lastFxEmit = now;

  const slot = fxWatched;
  if (slot === -1) return;
  const plugin = fxLoaded[slot];
  if (!plugin || plugin.params.length === 0) return;

  const values = plugin.params.map((p, i) => {
    if (i === 0) return 0.5 + 0.45 * Math.sin(now / 900);
    return fxValues[slot][i] ?? p.def;
  });
  emit("fxParamValues", {
    slot,
    values,
    texts: values.map((v, i) => plugin.params[i]!.format(v)),
  });
}

function startFxMeter(): void {
  if (fxRaf || typeof requestAnimationFrame !== "function") return;
  fxRaf = requestAnimationFrame(fxTick);
}

function stopFxMeter(): void {
  if (!fxRaf) return;
  cancelAnimationFrame(fxRaf);
  fxRaf = 0;
}

/* ───────────────────────── fake editor window ──────────────────────────── */

/**
 * The plugin window, emulated.
 *
 * In the plugin the WebView *is* the window and C++ owns its bounds. In a
 * browser nothing can resize the tab, so the mock keeps a size of its own and
 * the app shell renders at it (App.tsx writes it onto `--app-w`/`--app-h`).
 * That is what makes the grip, the clamps and the fluid layout testable in dev
 * — a grip that only ever reported "already at max" would test nothing.
 *
 * The browser viewport stands in for the display: a window bigger than it could
 * not be dragged back, which is exactly what `maxWidth`/`maxHeight` mean.
 */
const MIN_EDITOR_W = 900;
const MIN_EDITOR_H = 600;

function clampNum(value: number, min: number, max: number): number {
  return value < min ? min : value > max ? max : value;
}

function displayLimits(): { maxWidth: number; maxHeight: number } {
  return {
    maxWidth: Math.max(MIN_EDITOR_W, Math.floor(window.innerWidth)),
    maxHeight: Math.max(MIN_EDITOR_H, Math.floor(window.innerHeight)),
  };
}

/** Floor imposed by an embedded plugin; {0,0} when nothing is embedded. */
let embedFloor = { width: 0, height: 0 };

function setEmbedFloor(width: number, height: number): void {
  embedFloor = { width, height };
  const current = state.editorSize;
  applyEditorSize(Math.max(current.width, width), Math.max(current.height, height));
}

function editorSizeFor(width: number, height: number): EditorSizeLimits {
  const { maxWidth, maxHeight } = displayLimits();
  /* The reported minimum is the EFFECTIVE one, matching C++: while a plugin is
     embedded the window cannot shrink below what that plugin needs, and the grip
     clamps against exactly these numbers. */
  const minWidth = Math.min(Math.max(MIN_EDITOR_W, embedFloor.width), maxWidth);
  const minHeight = Math.min(Math.max(MIN_EDITOR_H, embedFloor.height), maxHeight);

  return {
    width: Math.round(clampNum(width, minWidth, maxWidth)),
    height: Math.round(clampNum(height, minHeight, maxHeight)),
    minWidth,
    minHeight,
    maxWidth,
    maxHeight,
  };
}

/** Opens filling the "display", like a plugin window the host has just sized. */
function initialEditorSize(): EditorSizeLimits {
  return editorSizeFor(window.innerWidth, window.innerHeight);
}

function sameEditorSize(a: EditorSizeLimits, b: EditorSizeLimits): boolean {
  return (
    a.width === b.width &&
    a.height === b.height &&
    a.minWidth === b.minWidth &&
    a.minHeight === b.minHeight &&
    a.maxWidth === b.maxWidth &&
    a.maxHeight === b.maxHeight
  );
}

/** Applies + broadcasts, exactly like C++: the event fires for our own writes
 *  too, so the page never has to remember what it asked for. */
function applyEditorSize(width: number, height: number): EditorSize {
  const next = editorSizeFor(width, height);
  const changed = !sameEditorSize(next, state.editorSize);
  state.editorSize = next;
  if (changed) emit("editorSizeChanged", { ...next });
  return { width: next.width, height: next.height };
}

/** Until the grip is used the emulated window tracks the browser window, the
 *  way a plugin window tracks the size the host gave it. */
let editorSizeDragged = false;

window.addEventListener("resize", () => {
  if (editorSizeDragged)
    applyEditorSize(state.editorSize.width, state.editorSize.height);
  else applyEditorSize(window.innerWidth, window.innerHeight);
});

/* ─────────────────────── embedded hosted editor ────────────────────────── */

/** Timers for a mount in flight — a second request must cancel the first. */
let embedTimers: number[] = [];

function clearEmbedTimers(): void {
  for (const id of embedTimers) window.clearTimeout(id);
  embedTimers = [];
}

function setEmbed(next: FxEmbedState): void {
  state.fxEmbed = next;
  emit("fxEmbedChanged", { ...next });
}

/**
 * What the page last said about the hole.
 *
 * Nothing composites above a browser page, so there is no native view to move
 * or hide; the mock records the calls and publishes them as
 * `window.__tubampEmbed`, which is how you check in dev that the rectangle
 * really does track the layout and that opening a menu really does hide it.
 */
const embedReport = {
  rect: { x: 0, y: 0, width: 0, height: 0 },
  visible: true,
  minWindow: { width: 0, height: 0 },
};
(window as unknown as { __tubampEmbed: typeof embedReport }).__tubampEmbed =
  embedReport;

/** Unmount when the slot's plugin goes away under it, as the real host must. */
function dropEmbedFor(slot: FxSlotIndex): void {
  if (state.fxEmbed.slot !== slot) return;
  clearEmbedTimers();
  setEmbed({ slot: -1, width: 0, height: 0, error: "" });
}

/* ────────────────────────────── mock plugin state ──────────────────────── */

const MODELS_DIR = "/Users/dev/Music/tubamp/models";
const IRS_DIR = "/Users/dev/Music/tubamp/irs";
const PRESETS_DIR = "/Users/dev/Music/tubamp/presets";

/**
 * `metadata.gear_type` per mock capture, covering all three states the real
 * library shows: an amp-only capture, two rigs taken through a cab, and a
 * capture that declares no gear at all (which must stay silent, not read as
 * amp-only). Keyed by display name; anything missing is the silent case.
 */
const MOCK_GEAR: Record<string, string> = {
  "Marshall JCM800 Crunch": "amp",
  "Fender Twin Clean": "amp",
  "Mesa Rectifier Modern": "amp_cab",
  "Vox AC30 Top Boost": "amp_mic",
};

/** Mirrors `gearIncludesCab()` in src/dsp/NamEngine.cpp. */
function mockGear(name: string): Pick<ModelInfo, "gearType" | "includesCab"> {
  const gearType = MOCK_GEAR[name];
  return {
    gearType,
    includesCab: gearType !== undefined && /cab|mic|full_rig/.test(gearType),
  };
}

const loadedModel: ModelInfo = {
  path: `${MODELS_DIR}/Marshall JCM800 Crunch.nam`,
  name: "Marshall JCM800 Crunch",
  sampleRateHz: 48000,
  loudnessDb: -13.4,
  inputLevelDbu: 12.5,
  outputLevelDbu: 11.2,
  ...mockGear("Marshall JCM800 Crunch"),
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
  t3k: { configured: true, authenticated: true, username: "mock_user" },
  // The fx tokens are absent from chainOrder above because they are absent from
  // chain::defaultOrder() — a slot only enters the path when the user adds it.
  fxSlots: initialFxSlots(),
  editorSize: initialEditorSize(),
  fxEmbed: { slot: -1, width: 0, height: 0, error: "" },
  fxSupported: true,
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

/**
 * Fake TONE3000 catalog behind `t3kBrowse` / `t3kListModels` / `t3kSetFavorite`.
 * It obeys the same rules the C++ client does (docs/research/tone3000-api.md):
 * `kind` splits nam from ir, `shelf: "favorites"` is a server-side shelf rather
 * than a client filter, and pages are 25 rows.
 */
const PAGE_SIZE = 25;

type ToneSeed = [
  title: string,
  creator: string,
  format: "nam" | "ir",
  gear: string,
  description: string,
  downloads: number,
  favorites: number,
  makes: string[],
  tags: string[],
  sizes: string[],
  createdAt: string,
];

const TONE_SEEDS: ToneSeed[] = [
  ["Deluxe Reverb '65 — Edge of Breakup", "amalgam_audio", "nam", "amp", "Blackface AB763 captured at the sweet spot, vol 6. Touch-sensitive cleans that grit up when you dig in.", 48213, 3120, ["Fender"], ["clean", "blackface", "breakup"], ["standard", "lite", "feather"], "2026-03-14"],
  ["JCM800 2203 — Full Stack Roar", "tonehound", "nam", "amp", "1982 JCM800, master at 8. The rock rhythm channel. Pairs best with a greenback IR.", 61540, 4470, ["Marshall"], ["rock", "crunch", "british"], ["standard", "lite"], "2025-11-02"],
  ["AC30 Top Boost — Chime Machine", "brit_captures", "nam", "amp", "Vox AC30/6 TB, cut at noon. Jangle and chime with EL84 compression when pushed.", 35872, 2910, ["Vox"], ["chime", "british", "cleanish"], ["standard", "lite", "feather"], "2026-01-20"],
  ["Dual Rectifier — Modern Scoop", "murkytones", "nam", "amp", "3-channel Recto, modern voicing, bold power. Tight palm mutes, huge low end.", 52108, 3350, ["Mesa/Boogie"], ["metal", "high-gain", "scooped"], ["standard", "lite"], "2025-09-18"],
  ["5150 Block Letter — Lead Channel", "riffworks", "nam", "amp", "The classic brown sound sequel. Lead channel, gain at 6, resonance 4.", 44930, 3860, ["Peavey"], ["high-gain", "lead", "hot-rodded"], ["standard", "lite", "feather"], "2026-05-30"],
  ["Princeton Reverb — Bedroom Cream", "amalgam_audio", "nam", "amp-cab", "Full-rig capture: '68 Princeton into its stock 10\" through a 57. Instant record-ready clean.", 29804, 2150, ["Fender"], ["clean", "full-rig", "lofi"], ["standard", "feather"], "2026-06-25"],
  ["Plexi Super Lead — Vintage Growl", "tonehound", "nam", "amp", "1969 Super Lead jumped channels. Woody midrange growl, cleans up beautifully off the guitar volume.", 38455, 3020, ["Marshall"], ["classic-rock", "plexi", "vintage"], ["standard", "lite"], "2025-12-11"],
  ["Bassman '59 — Tweed Warmth", "vintage_vault", "nam", "amp", "Tweed Bassman 4×10 combo head section. The circuit every amp copied. Warm, midrangey push.", 21600, 1740, ["Fender"], ["tweed", "vintage", "blues"], ["standard", "lite", "feather"], "2026-02-08"],
  ["SLO-100 — Liquid Lead", "labtones", "nam", "amp", "Soldano SLO overdrive channel. Smooth, singing sustain — the LA session lead sound.", 26377, 2280, ["Soldano"], ["lead", "high-gain", "smooth"], ["standard", "lite"], "2026-04-19"],
  ["BE-100 — Modern Brit Crunch", "riffworks", "nam", "amp", "Friedman BE channel, structure tight. Hot-rodded Marshall DNA with modern focus.", 31240, 2540, ["Friedman"], ["crunch", "modern", "british"], ["standard", "lite", "feather"], "2026-07-12"],
  ["JC-120 — Glass Cleans", "murkytones", "nam", "amp", "Roland Jazz Chorus, bright switch on. The flattest, glassiest solid-state clean ever made.", 18922, 1490, ["Roland"], ["clean", "solid-state", "glassy"], ["standard", "feather", "nano"], "2026-07-28"],
  ["Rockerverb 50 — Citrus Grind", "brit_captures", "nam", "amp", "Orange Rockerverb dirty channel. Thick, saturated British grind with that Orange midrange bark.", 15733, 1180, ["Orange"], ["stoner", "crunch", "thick"], ["standard", "lite"], "2026-07-20"],
  ["Ecstasy Blue — Boutique Drive", "labtones", "nam", "amp", "Bogner Ecstasy blue channel, plexi mode. Refined boutique overdrive with 3D depth.", 12048, 986, ["Bogner"], ["boutique", "drive", "dynamic"], ["standard", "lite", "feather"], "2026-07-31"],
  ["Dumble ODS — Holy Grail", "vintage_vault", "nam", "amp", "Overdrive Special #124 clone, OD channel. The most-imitated boutique voice, captured direct.", 40120, 4980, ["Dumble"], ["boutique", "smooth", "grail"], ["standard"], "2026-06-02"],
  ["Klon-Boosted Plexi Stack", "riffworks", "nam", "amp-cab", "Full-rig: Klon into '71 plexi into 4×12. Ready-to-play classic rock lead rig.", 24310, 1870, ["Marshall", "Klon"], ["full-rig", "boosted", "lead"], ["standard", "lite"], "2026-07-05"],
  ["Twin Reverb — Pedal Platform", "amalgam_audio", "nam", "amp", "'72 Twin at stage volume. Massive clean headroom — the definitive pedal platform.", 33590, 2400, ["Fender"], ["clean", "headroom", "platform"], ["standard", "lite", "feather"], "2025-10-27"],
  ["Matchless DC30 — Class A Sparkle", "brit_captures", "nam", "amp", "DC30 channel 1, EF86 voice. Chimey class-A sparkle that sits perfectly in a mix.", 9840, 812, ["Matchless"], ["boutique", "chime", "class-a"], ["standard", "lite"], "2026-08-01"],
  ["Two-Rock Classic — Studio Clean", "labtones", "nam", "amp", "Classic Reverb Signature. Big, bold, hi-fi cleans with Dumble lineage.", 11220, 940, ["Two-Rock"], ["clean", "boutique", "hifi"], ["standard", "feather"], "2026-07-25"],
  ["RAT on the Edge", "pedal_lab", "nam", "pedal", "ProCo RAT distortion pedal capture, filter at 2 o'clock. Stack it in front of a clean amp model.", 8102, 640, ["ProCo"], ["pedal", "distortion", "stackable"], ["standard", "feather", "nano"], "2026-07-15"],
  ["Tube Screamer TS808 — Mid Hump", "pedal_lab", "nam", "pedal", "The green machine at drive 3 / level 7. The eternal boost. A2 capture, nano available.", 14980, 1120, ["Ibanez"], ["pedal", "overdrive", "boost"], ["standard", "nano"], "2026-06-18"],
  ["4×12 Greenback — 57+121 Blend", "ir_foundry", "ir", "cab", "1971 basketweave with G12M-25s. SM57 + R-121 blended at the cone/cap seam. The rock IR.", 55214, 5310, ["Marshall", "Celestion"], ["4x12", "greenback", "blend"], ["standard"], "2025-12-01"],
  ["2×12 Alnico Blue — Vintage 30 Mix", "ir_foundry", "ir", "cab", "Open-back 2×12, Blue + V30 mix. Chime with body — made for AC30-style captures.", 31780, 2670, ["Vox", "Celestion"], ["2x12", "alnico", "open-back"], ["standard"], "2026-02-14"],
  ["Mesa OS 4×12 — V30 Tight", "cab_army", "ir", "cab", "Oversized Recto cab, quad V30s, 57 slightly off-axis. Tight low end for high gain.", 42350, 3240, ["Mesa/Boogie"], ["4x12", "v30", "metal"], ["standard"], "2026-01-09"],
  ["1×12 Deluxe — Royer Room", "studio_irs", "ir", "cab", "Oxford 12K5-6 in a '65 Deluxe combo, R-121 plus a touch of room. Vintage air.", 19240, 1580, ["Fender", "Oxford"], ["1x12", "vintage", "room"], ["standard"], "2026-05-22"],
  ["4×10 Bassman Tweed Stack", "vintage_vault", "ir", "cab", "All four Jensen P10Rs blended. Tweed spank and grind, captured with vintage ribbons.", 12490, 1050, ["Fender", "Jensen"], ["4x10", "tweed", "jensen"], ["standard"], "2026-06-30"],
  ["2×12 Lone Star — Fat & Wide", "cab_army", "ir", "cab", "Mesa Lone Star 2×12, MC90s, stereo-wide dual-mic. Big fat cleans and leads.", 8420, 720, ["Mesa/Boogie"], ["2x12", "wide", "clean"], ["standard"], "2026-07-27"],
  ["112 Blue Alnico — Close 57", "studio_irs", "ir", "cab", "Single Celestion Blue in a Deluxe-style cab. The chime cap, close-miked and bright.", 15110, 1310, ["Celestion"], ["1x12", "alnico", "bright"], ["standard"], "2026-07-19"],
  ["4×12 Uber — T75/V30 X-Pattern", "ir_foundry", "ir", "cab", "Bogner Uberkab X-pattern quad. Scooped T75 sizzle plus V30 mids in one IR.", 22870, 1740, ["Bogner", "Celestion"], ["4x12", "x-pattern", "modern"], ["standard"], "2026-04-03"],
  ["Pine 1×10 — Lo-Fi Character", "studio_irs", "ir", "cab", "Vintage pine 1×10 with a worn ceramic speaker. Boxy on purpose — instant character.", 4310, 386, ["Supro"], ["1x10", "lofi", "character"], ["standard"], "2026-08-02"],
];

/** Inline thumbnail: exercises the <img> path, which a data URI reaches even
 *  though the WebView origin blocks remote images. */
function thumbnail(from: string, to: string): string {
  const svg =
    `<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96">` +
    `<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1">` +
    `<stop offset="0" stop-color="${from}"/><stop offset="1" stop-color="${to}"/>` +
    `</linearGradient></defs><rect width="96" height="96" fill="url(#g)"/></svg>`;
  return `data:image/svg+xml,${encodeURIComponent(svg)}`;
}

/** Every third tone carries art, one carries a dead URL, the rest none — so the
 *  drawer's three art paths (image, broken image, no image) are all reachable. */
function artFor(index: number): string | null {
  if (index === 4) return "https://tone3000.test/missing.png";
  if (index % 3 !== 0) return null;
  const hue = (index * 47) % 360;
  return thumbnail(`hsl(${hue} 55% 42%)`, `hsl(${(hue + 40) % 360} 45% 12%)`);
}

function seedToTone(seed: ToneSeed, id: number, index: number): T3kTone {
  const [title, username, format, gear, description, downloadsCount, favoritesCount, makes, tags, sizes, createdAt] = seed;
  return {
    id,
    title,
    description,
    gear,
    format,
    imageUrl: artFor(index),
    creator: { username, avatarUrl: null },
    downloadsCount,
    favoritesCount,
    favorited: false,
    makes,
    tags,
    sizes,
    modelsCount: format === "ir" ? 1 + (title.length % 3) : sizes.length,
    createdAt,
  };
}

/* Each nam tone also ships a second take: the catalog has to span more than one
   25-row page for the "Load more" row to be reachable in dev. */
const CATALOG: T3kTone[] = TONE_SEEDS.flatMap((seed, i) => {
  const tone = seedToTone(seed, 100 + i, i);
  if (tone.format === "ir") return [tone];
  return [
    tone,
    {
      ...tone,
      id: tone.id + 1000,
      title: `${tone.title} (take 2)`,
      downloadsCount: Math.round(tone.downloadsCount * 0.4),
      favoritesCount: Math.round(tone.favoritesCount * 0.35),
      imageUrl: artFor(i + 1),
      createdAt: "2026-08-03",
    },
  ];
});

const favorites = new Set<number>([101, 113, 120, 1104]);

/** Deterministic trending score — recent-ish downloads bias, stable per tone. */
function trendScore(tone: T3kTone): number {
  return (
    tone.downloadsCount * (0.6 + ((tone.id * 37) % 100) / 100) +
    Date.parse(tone.createdAt) / 3.6e7
  );
}

function browseCatalog(request: T3kBrowseRequest): T3kBrowseResult {
  const needle = request.query.trim().toLowerCase();
  let rows = CATALOG.filter((t) =>
    request.kind === "irs" ? t.format === "ir" : t.format === "nam",
  );
  if (request.shelf === "favorites") rows = rows.filter((t) => favorites.has(t.id));
  if (request.gear) rows = rows.filter((t) => t.gear === request.gear);
  if (needle)
    rows = rows.filter((t) =>
      [t.title, t.description, t.creator.username, ...t.makes, ...t.tags]
        .join(" ")
        .toLowerCase()
        .includes(needle),
    );

  const sorted = [...rows];
  if (request.sort === "newest")
    sorted.sort((a, b) => Date.parse(b.createdAt) - Date.parse(a.createdAt));
  else if (request.sort === "downloads")
    sorted.sort((a, b) => b.downloadsCount - a.downloadsCount);
  else sorted.sort((a, b) => trendScore(b) - trendScore(a));

  const totalPages = Math.max(1, Math.ceil(sorted.length / PAGE_SIZE));
  const page = Math.min(Math.max(1, request.page), totalPages);
  return {
    tones: sorted
      .slice((page - 1) * PAGE_SIZE, page * PAGE_SIZE)
      .map((t) => ({ ...t, favorited: favorites.has(t.id) })),
    page,
    totalPages,
    total: sorted.length,
  };
}

/** Model ids are derived from the tone id so a download's identity survives
 *  re-listing a tone. */
function listToneModels(toneId: number): T3kModel[] {
  const tone = CATALOG.find((t) => t.id === toneId);
  if (!tone) return [];
  if (tone.format === "ir") {
    return Array.from({ length: tone.modelsCount }, (_, i) => ({
      id: toneId * 10 + i,
      name: `${tone.title} — position ${i + 1}`,
      modelUrl: `https://tone3000.test/m/${toneId * 10 + i}.wav`,
      size: "standard",
      architecture: "",
      kind: "wav" as const,
    }));
  }
  return tone.sizes.map((size, i) => ({
    id: toneId * 10 + i,
    name: `${tone.title} (${size})`,
    modelUrl: `https://tone3000.test/m/${toneId * 10 + i}.nam`,
    size,
    architecture: "2",
    kind: "nam" as const,
  }));
}

/* Select-flow tones (the `t3kStartSelectFlow` path, unchanged): the last one has
   no models, which the native UI failed silently on and the React UI toasts. */
const T3K_TONES: { toneId: number; models: T3kModel[] }[] = [
  { toneId: 101, models: listToneModels(101) },
  { toneId: 102, models: listToneModels(102) },
  { toneId: 60002, models: [] },
];

let nextTone = 0;
const downloads = new Map<number, number>();

/** `?t3kfail` makes every simulated download die half-way, so the failure path
 *  (t3kError carrying a modelId, progress row cleared) is reachable in dev;
 *  `?t3kfavfail` does the same for the optimistic favorite toggle. */
const search = new URLSearchParams(window.location.search);
const failDownloads = search.has("t3kfail");
const failFavorites = search.has("t3kfavfail");

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
    // `kind` routes the file exactly as the C++ downloader does: models/ for
    // captures, irs/ for impulse responses.
    const isIr = model.kind === "wav";
    const entry: FileEntry = {
      path: `${isIr ? IRS_DIR : MODELS_DIR}/${model.name}.${model.kind}`,
      name: model.name,
    };
    const shelf = isIr ? state.irs : state.models;
    if (!shelf.some((f) => f.path === entry.path)) {
      if (isIr) state.irs = [...state.irs, entry];
      else state.models = [...state.models, entry];
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
      ...mockGear(entry.name),
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
      ...mockGear(entry.name),
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
    state.t3k = { configured: true, authenticated: true, username: "mock_user" };
    emit("t3kStatus", { ...state.t3k });
    return {} satisfies ErrorResult;
  },

  // Signing out forgets the tokens, not the publishable key (Tone3000Client::
  // signOut) — which is also what makes the drawer's sign-in splash reachable.
  t3kSignOut: async () => {
    state.t3k = { ...state.t3k, authenticated: false, username: null };
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

  t3kSignIn: async () => {
    if (!state.t3k.configured) {
      await delay(null, 200);
      emit("t3kError", { message: "Add your TONE3000 key in Settings first." });
      return;
    }
    await delay(null, 800); // system browser round-trip
    state.t3k = { ...state.t3k, authenticated: true, username: "mock_user" };
    emit("t3kStatus", { ...state.t3k });
  },

  t3kBrowse: async (request: T3kBrowseRequest) => {
    if (!state.t3k.authenticated)
      return delay<T3kBrowseResult>({ error: "Not signed in to TONE3000." }, 80);
    return delay(browseCatalog(request), 220);
  },

  t3kListModels: async (toneId) => {
    const models = listToneModels(toneId);
    if (models.length === 0)
      return delay<T3kModelsResult>(
        { error: "That tone has no downloadable files." },
        180,
      );
    return delay<T3kModelsResult>({ models }, 180);
  },

  t3kSetFavorite: async (toneId, favorite) => {
    if (failFavorites)
      return delay<ErrorResult>({ error: "TONE3000 rejected the favorite." }, 150);
    // PUT/DELETE /tones/{id}/favorite are idempotent, so the count only moves
    // when the flag actually changes.
    const changed = favorite !== favorites.has(toneId);
    if (favorite) favorites.add(toneId);
    else favorites.delete(toneId);
    if (changed) {
      for (const tone of CATALOG) {
        if (tone.id === toneId)
          tone.favoritesCount = Math.max(0, tone.favoritesCount + (favorite ? 1 : -1));
      }
    }
    return delay<ErrorResult>({}, 150);
  },

  /* --- external AudioUnit slots ------------------------------------------- */

  fxListPlugins: async () => {
    const plugins: FxPluginEntry[] = FX_PLUGINS.map(
      ({ identifier, name, manufacturer, version }) => ({
        identifier,
        name,
        manufacturer,
        version,
      }),
    );
    // A registry walk, not an instantiation — fast even with a big plugin folder.
    return delay({ plugins, supported: state.fxSupported }, 90);
  },

  fxLoad: async (slot: FxSlotIndex, identifier) => {
    const plugin = findFxPlugin(identifier);
    if (!plugin)
      return delay<ErrorResult>({ error: "That plugin is no longer installed." });

    fxLoaded[slot] = null;
    fxValues[slot] = [];
    dropEmbedFor(slot);
    emitFxSlot(slot, {
      ...emptyFxSlot(slot),
      identifier,
      name: plugin.name,
      manufacturer: plugin.manufacturer,
      occupied: true,
      loading: true,
      hasEditor: plugin.hasEditor,
    });

    // Instantiation is asynchronous in the real host (the AU is created on a
    // background thread and swapped in), and slow enough to be worth a state.
    await delay(null, 900);
    if (plugin.failsToLoad) {
      emitFxSlot(slot, {
        ...emptyFxSlot(slot),
        identifier,
        name: plugin.name,
        manufacturer: plugin.manufacturer,
        occupied: true,
        error: `${plugin.name} failed to initialise (AudioUnit error -10863).`,
      });
      return {} satisfies ErrorResult;
    }

    fxLoaded[slot] = plugin;
    fxValues[slot] = plugin.params.map((p) => p.def);
    emitFxSlot(slot, liveFxSlot(plugin, slot));
    return {} satisfies ErrorResult;
  },

  fxClear: async (slot: FxSlotIndex) => {
    fxLoaded[slot] = null;
    fxValues[slot] = [];
    dropEmbedFor(slot);
    await delay(null, 0);
    emitFxSlot(slot, emptyFxSlot(slot));
  },

  // There is no window to open in a browser; reporting it keeps the button's
  // failure path (a toast) exercised in dev.
  fxOpenEditor: async (slot: FxSlotIndex) =>
    delay<ErrorResult>(
      fxLoaded[slot]
        ? { error: "The mock bridge has no plugin window to open." }
        : { error: "Nothing is loaded in this slot." },
      120,
    ),

  fxSetParam: async (slot: FxSlotIndex, index, value) => {
    const plugin = fxLoaded[slot];
    if (!plugin || index < 0 || index >= plugin.params.length) return;
    fxValues[slot][index] = Math.min(1, Math.max(0, value));
    await delay(null, 0);
  },

  /* Host automation touch: nothing to tell outside the plugin. */
  fxBeginGesture: async () => {},
  fxEndGesture: async () => {},

  fxWatchSlot: async (slot) => {
    fxWatched = slot;
    if (slot < 0) stopFxMeter();
    else startFxMeter();
    await delay(null, 0);
  },

  /* --- window size --------------------------------------------------------- */

  getEditorSize: async () => delay({ ...state.editorSize }, 40),

  setEditorSize: async (width, height) => {
    editorSizeDragged = true;
    // Synchronous on purpose: this is called from a pointermove and the page
    // relays out on the event it emits. A delay here would show as grip lag.
    return applyEditorSize(width, height);
  },

  /* --- embedding a hosted plugin's own editor ------------------------------ */

  fxSetEmbedSlot: async (slot) => {
    clearEmbedTimers();

    if (slot === -1) {
      setEmbed({ slot: -1, width: 0, height: 0, error: "" });
      return {} satisfies ErrorResult;
    }

    const plugin = fxLoaded[slot];
    if (!plugin) {
      await delay(null, 60);
      return { error: "Nothing is loaded in this slot." } satisfies ErrorResult;
    }

    const { embed } = plugin;
    if (embed.refuse) {
      // Refused before anything mounts. Both channels carry it: the caller gets
      // the error to fall back on, and the event tells every other listener the
      // embed is not coming.
      await delay(null, 120);
      setEmbed({ slot: -1, width: 0, height: 0, error: embed.refuse });
      return { error: embed.refuse } satisfies ErrorResult;
    }

    // Creating the view and attaching it takes a beat, and a plugin that only
    // knows its real size afterwards reports twice.
    const first = embed.provisional ?? embed;
    embedTimers.push(
      window.setTimeout(
        () =>
          setEmbed({
            slot,
            width: first.width,
            height: first.height,
            error: "",
          }),
        160,
      ),
    );
    if (embed.provisional) {
      embedTimers.push(
        window.setTimeout(
          () =>
            setEmbed({
              slot,
              width: embed.width,
              height: embed.height,
              error: "",
            }),
          900,
        ),
      );
    }
    return {} satisfies ErrorResult;
  },

  fxSetEmbedRect: async (x, y, width, height) => {
    embedReport.rect = { x, y, width, height };
    await delay(null, 0);
  },

  fxSetEmbedMinWindow: async (width, height) => {
    embedReport.minWindow = { width, height };
    /* Same contract as C++: this is a floor, not a request. It raises the limits
       the grip clamps against and grows once if we are already smaller — never a
       reactive loop chasing its own resize events. */
    setEmbedFloor(width, height);
    await delay(null, 0);
  },

  fxSetEmbedVisible: async (visible) => {
    embedReport.visible = visible;
    await delay(null, 0);
  },
};
