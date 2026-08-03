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
] as const;

export type BlockId = (typeof BLOCK_IDS)[number];

export function isBlockId(value: string): value is BlockId {
  return (BLOCK_IDS as readonly string[]).includes(value);
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

/** 10 toggle params. */
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
}
