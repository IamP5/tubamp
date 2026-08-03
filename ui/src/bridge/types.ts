/**
 * THE bridge contract (docs/REACT-UI.md §Bridge contract).
 *
 * Everything the UI knows about the plugin passes through these types. The real
 * implementation (./juce.ts) and the mock (./mock.ts) both satisfy `Bridge`;
 * `./index.ts` picks one at module load. Feature code must import from
 * `../bridge` — never from `../juce/index.js` directly.
 */

/* ────────────────────────────── chain blocks ───────────────────────────── */

/** Persisted chain tokens (`chain::BlockInfo::token`), in `defaultOrder()` order. */
export const BLOCK_IDS = [
  "gate",
  "comp",
  "drive",
  "amp",
  "cab",
  "eq",
  "mod",
  "delay",
  "reverb",
  "fx1",
  "fx2",
  "fx3",
] as const;

export type BlockId = (typeof BLOCK_IDS)[number];

export function isBlockId(value: string): value is BlockId {
  return (BLOCK_IDS as readonly string[]).includes(value);
}

/** The three external-AudioUnit slots, in slot-index order. */
export const FX_BLOCK_IDS = ["fx1", "fx2", "fx3"] as const;

export type FxBlockId = (typeof FX_BLOCK_IDS)[number];

export function isFxBlockId(value: string): value is FxBlockId {
  return (FX_BLOCK_IDS as readonly string[]).includes(value);
}

/** 0 | 1 | 2 for an fx block, -1 otherwise. Mirrors chain::fxSlotIndex. */
export function fxSlotIndexOf(block: BlockId): FxSlotIndex | -1 {
  const index = (FX_BLOCK_IDS as readonly string[]).indexOf(block);
  return index < 0 ? -1 : (index as FxSlotIndex);
}

/* ─────────────────────────── parameter identifiers ─────────────────────── */

/** 29 slider params — ids are the APVTS ids verbatim and are frozen. */
export const SLIDER_PARAM_IDS = [
  "input_trim",
  "output_level",
  "gate_threshold",
  "comp_threshold",
  "comp_ratio",
  "comp_attack",
  "comp_release",
  "comp_makeup",
  "drive_gain",
  "drive_tone",
  "drive_level",
  "amp_input",
  "amp_output",
  "amp_cal_level",
  "amp_slim",
  "cab_lowcut",
  "cab_highcut",
  "eq_bass",
  "eq_mid",
  "eq_treble",
  "mod_rate",
  "mod_depth",
  "mod_mix",
  "delay_time",
  "delay_feedback",
  "delay_mix",
  "reverb_size",
  "reverb_damping",
  "reverb_mix",
] as const;

/** 13 toggle params. */
export const TOGGLE_PARAM_IDS = [
  "gate_on",
  "comp_on",
  "drive_on",
  "amp_on",
  "cab_on",
  "eq_on",
  "mod_on",
  "delay_on",
  "reverb_on",
  "amp_cal_input",
  "fx1_on",
  "fx2_on",
  "fx3_on",
] as const;

/** 2 combo params. */
export const COMBO_PARAM_IDS = ["amp_out_mode", "mod_type"] as const;

export type SliderParamId = (typeof SLIDER_PARAM_IDS)[number];
export type ToggleParamId = (typeof TOGGLE_PARAM_IDS)[number];
export type ComboParamId = (typeof COMBO_PARAM_IDS)[number];
export type ParamId = SliderParamId | ToggleParamId | ComboParamId;

/* ───────────────────────────── param state shapes ──────────────────────── */

export interface SliderProperties {
  start: number;
  end: number;
  skew: number;
  name: string;
  label: string;
  numSteps: number;
  interval: number;
  parameterIndex: number;
}

export interface ToggleProperties {
  name: string;
  parameterIndex: number;
}

export interface ComboProperties {
  name: string;
  parameterIndex: number;
  choices: string[];
}

/** Unsubscribe callback returned by every subscribe-shaped bridge method. */
export type Unsubscribe = () => void;

export interface SliderParamState {
  readonly properties: SliderProperties;
  /** 0..1, skew applied. */
  getNormalisedValue(): number;
  /** Parameter units (dB / Hz / ms / …). */
  getScaledValue(): number;
  setNormalisedValue(value: number): void;
  /** Host automation touch — wrap every gesture. */
  beginGesture(): void;
  endGesture(): void;
  onValueChanged(listener: () => void): Unsubscribe;
  onPropertiesChanged(listener: () => void): Unsubscribe;
}

export interface ToggleParamState {
  readonly properties: ToggleProperties;
  getValue(): boolean;
  setValue(value: boolean): void;
  onValueChanged(listener: () => void): Unsubscribe;
  onPropertiesChanged(listener: () => void): Unsubscribe;
}

export interface ComboParamState {
  readonly properties: ComboProperties;
  getChoiceIndex(): number;
  setChoiceIndex(index: number): void;
  onValueChanged(listener: () => void): Unsubscribe;
  onPropertiesChanged(listener: () => void): Unsubscribe;
}

/* ─────────────────────────────── UI state ──────────────────────────────── */

export interface ModelInfo {
  path: string;
  name: string;
  sampleRateHz: number;
  loudnessDb?: number;
  inputLevelDbu?: number;
  outputLevelDbu?: number;
  /** `metadata.gear_type` from the .nam, lower_snake (`amp`, `amp_cab`,
   *  `pedal_amp_mic`, …). Absent when the capture declares none — most files
   *  trained outside TONE3000's exporter carry no gear fields, and absence must
   *  never be read as "amp only". */
  gearType?: string;
  /** Derived C++-side from `gearType`: the capture already went through a
   *  cabinet, so the Cab block would stack a second one on top of it. */
  includesCab: boolean;
  isSlimmable: boolean;
  latencySamples: number;
}

export interface FileEntry {
  path: string;
  name: string;
}

export interface PresetInfo {
  name: string;
  path: string;
  favorite: boolean;
  tags: string[];
}

export type AbSlot = 0 | 1;

export interface AbState {
  activeSlot: AbSlot;
  aHasState: boolean;
  bHasState: boolean;
}

export interface T3kState {
  configured: boolean;
  authenticated: boolean;
  /** TONE3000 username once known (fetched after sign-in); null before. */
  username: string | null;
}

/* ──────────────────── external AudioUnit slots (fx1..fx3) ──────────────────── */

export type FxSlotIndex = 0 | 1 | 2;

/** One installed AudioUnit effect, as listed by the picker. Discovered from the
 *  component registry without instantiating anything, so this list is cheap and
 *  never runs third-party code. */
export interface FxPluginEntry {
  /** `PluginDescription::fileOrIdentifier` — the only field needed to load it. */
  identifier: string;
  name: string;
  manufacturer: string;
  version: string;
}

/** One parameter of a hosted plugin. Unlike tubamp's own parameters these are
 *  discovered at load time, so they carry their own metadata rather than being
 *  looked up in a frozen table. */
export interface FxParamInfo {
  index: number;
  name: string;
  /** Unit suffix the plugin reports ("dB", "Hz", "%"), often empty. */
  label: string;
  /** Normalised 0..1 — the only representation a hosted parameter guarantees. */
  value: number;
  /** The plugin's own formatted display string for `value`. */
  text: string;
}

export interface FxSlotState {
  slot: FxSlotIndex;
  /** Empty when no plugin is assigned. */
  identifier: string;
  name: string;
  manufacturer: string;
  /** A plugin is assigned to this slot. */
  occupied: boolean;
  /** Assigned but not runnable here — almost always "not installed on this
   *  machine". Its saved settings are preserved and re-saved untouched. */
  missing: boolean;
  /** An instance is actually processing. */
  live: boolean;
  /** Instantiation in flight. */
  loading: boolean;
  latencySamples: number;
  /** Why the slot is not live, when it isn't. */
  error: string;
  /** The plugin ships its own editor window (as opposed to a generic one). */
  hasEditor: boolean;
  /** Empty until the plugin is live. */
  params: FxParamInfo[];
}

/* ─────────────────────────── window size & embedding ───────────────────────── */

export interface EditorSize {
  width: number;
  height: number;
}

export interface EditorSizeLimits extends EditorSize {
  minWidth: number;
  minHeight: number;
  /** Largest size that actually fits the display the window is on — not an
   *  arbitrary ceiling. Embedding a plugin bigger than this is refused. */
  maxWidth: number;
  maxHeight: number;
}

/** Which slot's plugin is currently drawn inside our window, and how big its editor
 *  turned out to be. `slot` is -1 when nothing is embedded. */
export interface FxEmbedState {
  slot: FxSlotIndex | -1;
  /** The hosted editor's own size. Provisional: some plugins only report a real
   *  size once their view has been attached to a window, and a few change it
   *  afterwards, so this can arrive more than once. */
  width: number;
  height: number;
  /** Non-empty when embedding was refused — the plugin needs more room than the
   *  display allows. The UI falls back to opening it in its own window. */
  error: string;
}

export interface UiState {
  /** Chain tokens in order; `[]` = deliberately empty chain (the "-" sentinel). */
  chainOrder: string[];
  model: ModelInfo | null;
  ir: FileEntry | null;
  models: FileEntry[];
  irs: FileEntry[];
  presets: PresetInfo[];
  currentPresetName: string;
  ab: AbState;
  t3k: T3kState;
  /** Always three entries, indexed by slot. */
  fxSlots: FxSlotState[];
  editorSize: EditorSizeLimits;
  fxEmbed: FxEmbedState;
  /** False in builds compiled without plugin hosting; the FX panels then explain
   *  themselves instead of showing an empty picker. */
  fxSupported: boolean;
}

export interface T3kModel {
  id: number;
  name: string;
  modelUrl: string;
  size: string;
  architecture: string;
  /** Derived C++-side from the file extension: .nam capture or .wav IR.
   *  Routes the download destination (models/ vs irs/) and the auto-load call
   *  (loadModel vs loadIr). */
  kind: "nam" | "wav";
}

/* ─────────────────────── TONE3000 in-plugin browser ────────────────────── */

export type T3kBrowseKind = "models" | "irs";
export type T3kBrowseShelf = "all" | "favorites";
export type T3kBrowseSort = "trending" | "newest" | "downloads";

/**
 * One page of the catalog. `kind` maps C++-side to
 * `format=nam&architecture=2` (models) or `format=ir` (irs) — omitting
 * `architecture` would silently exclude A2 (tone3000-api.md §6).
 * `shelf: "favorites"` reads the bounded `/tones/favorited` list instead of
 * `/tones/search` and filters/sorts client-side.
 */
export interface T3kBrowseRequest {
  kind: T3kBrowseKind;
  shelf: T3kBrowseShelf;
  sort: T3kBrowseSort;
  query: string;
  /** Gear filter (amp | amp-cab | pedal); models only, null = all. */
  gear: string | null;
  /** 1-based. */
  page: number;
}

export interface T3kCreator {
  username: string;
  avatarUrl: string | null;
}

export interface T3kTone {
  id: number;
  title: string;
  description: string;
  gear: string;
  format: "nam" | "ir";
  /** First catalog image; UI must fall back to a generated placeholder when
   *  null or when the remote image fails to load inside the WebView. */
  imageUrl: string | null;
  creator: T3kCreator;
  downloadsCount: number;
  favoritesCount: number;
  /** From the client's cached favorited-ids set, not the search payload. */
  favorited: boolean;
  makes: string[];
  tags: string[];
  sizes: string[];
  /** a2_models_count for nam tones; irs_count for ir tones. */
  modelsCount: number;
  createdAt: string;
}

export interface T3kBrowseResult {
  tones?: T3kTone[];
  page?: number;
  totalPages?: number;
  total?: number;
  error?: string;
}

export interface T3kModelsResult {
  models?: T3kModel[];
  error?: string;
}

/* ───────────────────────────────── events ─────────────────────────────── */

export interface BridgeEventMap {
  meters: { in: number; out: number };
  chainChanged: { chainOrder: string[] };
  libraryChanged: { models: FileEntry[]; irs: FileEntry[] };
  presetChanged: {
    presets: PresetInfo[];
    currentPresetName: string;
    ab: AbState;
  };
  modelChanged: { model: ModelInfo | null };
  irChanged: { ir: FileEntry | null };
  t3kStatus: T3kState;
  t3kToneSelected: { toneId: number; models: T3kModel[] };
  t3kProgress: { modelId: number; progress: number };
  t3kComplete: { modelId: number; path: string };
  /** `modelId` is present when the failure belongs to one model's download, so
   *  the store can drop that model's progress row; absent for select-flow and
   *  configuration errors, which have no model context. */
  t3kError: { message: string; modelId?: number };
  /** A slot's plugin, status or parameter list changed — anything structural. */
  fxSlotChanged: { slot: FxSlotIndex; state: FxSlotState };
  /** Parameter values only, pushed for the slot the user is looking at. Values and
   *  texts are index-aligned with that slot's `params`. Separate from
   *  `fxSlotChanged` because it fires at meter rate and must not churn the list. */
  fxParamValues: { slot: FxSlotIndex; values: number[]; texts: string[] };
  /** The window changed size — including when we changed it ourselves, so the
   *  page always has one source of truth. */
  editorSizeChanged: EditorSizeLimits;
  /** An embed mounted, unmounted, was refused, or reported a new size. */
  fxEmbedChanged: FxEmbedState;
}

export type BridgeEventName = keyof BridgeEventMap;

/* ─────────────────────────── native function results ───────────────────── */

export interface ErrorResult {
  error?: string;
}

export interface ImportResult {
  path?: string;
  name?: string;
  error?: string;
}

/* ─────────────────────────────── the bridge ────────────────────────────── */

/** Reports the parameter under the mouse to the host (Logic touch-to-select). */
export interface ParamIndexUpdater {
  handleMouseMove(event: MouseEvent): void;
}

export interface Bridge {
  /** "juce" when running inside the WebBrowserComponent, "mock" in a browser. */
  readonly kind: "juce" | "mock";

  /**
   * Pump mousemove through this and annotate controls with the attribute
   * (`data-param-index`) so the host knows which parameter is under the cursor.
   */
  paramIndexUpdater(attribute: string): ParamIndexUpdater;

  /* param access */
  sliderState(id: SliderParamId): SliderParamState;
  toggleState(id: ToggleParamId): ToggleParamState;
  comboState(id: ComboParamId): ComboParamState;

  /* events */
  on<E extends BridgeEventName>(
    event: E,
    listener: (payload: BridgeEventMap[E]) => void,
  ): Unsubscribe;

  /* native functions — names verbatim from the spec */
  getUiState(): Promise<UiState>;
  setChainOrder(tokens: string[]): Promise<void>;
  loadModel(path: string): Promise<ErrorResult>;
  clearModel(): Promise<void>;
  loadIr(path: string): Promise<ErrorResult>;
  clearIr(): Promise<void>;
  /** Native async FileChooser (.nam). Installs into the library *and* loads the
   *  file; callers must not follow up with `loadModel(path)`. */
  importModel(): Promise<ImportResult>;
  /** Native async FileChooser (.wav). Installs *and* loads, like importModel. */
  importIr(): Promise<ImportResult>;
  loadPreset(path: string): Promise<ErrorResult>;
  /** Native AlertWindow text prompt, then saves. */
  savePresetAs(): Promise<ErrorResult>;
  deletePreset(path: string): Promise<ErrorResult>;
  setPresetFavorite(path: string, favorite: boolean): Promise<void>;
  abCapture(slot: AbSlot): Promise<void>;
  abRecall(slot: AbSlot): Promise<void>;
  /** Native AlertWindow prompt for the publishable key. */
  t3kConfigure(): Promise<ErrorResult>;
  t3kSignOut(): Promise<void>;
  /** Async; results arrive as `t3kToneSelected` / `t3kError`. */
  t3kStartSelectFlow(): Promise<void>;
  /** Async; `t3kProgress` / `t3kComplete` / `t3kError` follow. */
  t3kDownloadModel(model: T3kModel): Promise<void>;
  /** Standard OAuth flow (system browser + loopback redirect), no tone
   *  selection. Outcome arrives as `t3kStatus` (success) / `t3kError`. */
  t3kSignIn(): Promise<void>;
  /** One catalog page. Resolves with data or `{error}`; never rejects. */
  t3kBrowse(request: T3kBrowseRequest): Promise<T3kBrowseResult>;
  /** Downloadable files of one tone (A2-filtered for nam tones). */
  t3kListModels(toneId: number): Promise<T3kModelsResult>;
  /** Favorite/unfavorite on TONE3000. UI updates optimistically and reverts
   *  on `{error}`. */
  t3kSetFavorite(toneId: number, favorite: boolean): Promise<ErrorResult>;

  /* --- external AudioUnit slots --------------------------------------------
   *
   * Hosted parameters do NOT go through JUCE relays. Relays need a static id at
   * editor-construction time, but a hosted plugin's parameter list is unknown
   * until it loads and changes when it is replaced. So these are plain native
   * calls out and a metered event back — which also means hosted parameters are
   * not visible to host automation in this version (the slot's own `fxN_on`
   * bypass is a normal APVTS parameter and automates as usual).
   */

  /** Installed AUv2 effects, sorted by manufacturer. Cheap: a component-registry
   *  walk that instantiates nothing. */
  fxListPlugins(): Promise<{ plugins: FxPluginEntry[]; supported: boolean }>;
  /** Assigns and instantiates asynchronously; `fxSlotChanged` reports the outcome.
   *  Resolving without an error only means the request was accepted. */
  fxLoad(slot: FxSlotIndex, identifier: string): Promise<ErrorResult>;
  fxClear(slot: FxSlotIndex): Promise<void>;
  /** Opens the plugin's own window — a native window, which is also the only way
   *  its text fields get keyboard focus inside a host like Logic. */
  fxOpenEditor(slot: FxSlotIndex): Promise<ErrorResult>;
  /** `value` is normalised 0..1. */
  fxSetParam(slot: FxSlotIndex, index: number, value: number): Promise<void>;
  /** Wrap drags so the plugin sees a proper gesture. */
  fxBeginGesture(slot: FxSlotIndex, index: number): Promise<void>;
  fxEndGesture(slot: FxSlotIndex, index: number): Promise<void>;
  /** Which slot should receive `fxParamValues` pushes; -1 for none. Set as the
   *  selected block changes so we only meter what is on screen. */
  fxWatchSlot(slot: FxSlotIndex | -1): Promise<void>;

  /* --- window size ---------------------------------------------------------
   *
   * The window is resized by US, never by the host. JUCE's AU wrapper reverts a
   * host-driven resize on the next parentSizeChanged, but propagates a
   * plugin-driven one through childBoundsChanged -> resizeHostWindow. And the
   * usual ResizableCornerComponent is useless here: it is a JUCE-painted child,
   * and the WebView is a native view that covers it. So the grip is drawn in the
   * page and calls setEditorSize.
   */

  getEditorSize(): Promise<EditorSizeLimits>;
  /** Clamped to the limits; resolves with what was actually applied. */
  setEditorSize(width: number, height: number): Promise<EditorSize>;

  /* --- embedding a hosted plugin's own editor -------------------------------
   *
   * The hosted editor is a native view that is a SIBLING of the WebView, not
   * part of the page. It composites above it, so the page cannot draw over the
   * rectangle it occupies — treat that rectangle as a hole: reserve it, keep
   * overlays clear of it, and hide it (fxSetEmbedVisible) whenever something
   * must appear on top.
   */

  /** -1 unmounts. Resolves with an error when the plugin cannot fit the display,
   *  in which case the caller should fall back to fxOpenEditor. */
  fxSetEmbedSlot(slot: FxSlotIndex | -1): Promise<ErrorResult>;
  /** Where the hole is, in CSS pixels relative to the page origin. */
  fxSetEmbedRect(x: number, y: number, width: number, height: number): Promise<void>;
  /** Hide without unmounting, for as long as an overlay needs to be on top. */
  fxSetEmbedVisible(visible: boolean): Promise<void>;
  /**
   * The smallest window that can still show the current embed — the plugin's own
   * size plus whatever chrome the page puts around it. Passing it here rather than
   * resizing on demand is deliberate: it becomes the resize floor, so the grip
   * simply stops at the embed's minimum instead of a grow-loop fighting the drag.
   * Also grows the window once, immediately, if it is currently smaller.
   * `0, 0` releases the floor.
   */
  fxSetEmbedMinWindow(width: number, height: number): Promise<void>;
}
