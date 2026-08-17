/**
 * THE bridge contract (docs/REACT-UI.md §Bridge contract).
 *
 * Everything the UI knows about the plugin passes through these types. The real
 * implementation (./juce.ts) and the mock (./mock.ts) both satisfy `Bridge`;
 * `./index.ts` picks one at module load. Feature code must import from
 * `../bridge` — never from `../juce/index.js` directly.
 */

/* ────────────────────────────── chain blocks ───────────────────────────── */

/**
 * Persisted chain tokens (`chain::BlockInfo::token`). A token names a block
 * INSTANCE, not a block type: the first twelve are the v1 ids and are frozen,
 * the twelve instance tokens are appended after them exactly as
 * `chain::BlockId` appends its new enumerators, and split/lane2/mix/amp2
 * (docs/SPLIT.md §1) close out the list — the exact tail order `ChainOrder.h`
 * appends its own enumerators in, `split == 24` pinned there too. Instance 1
 * of every kind keeps the legacy token (`comp`), so existing state loads
 * unchanged.
 */
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
  "comp2",
  "comp3",
  "drive2",
  "drive3",
  "eq2",
  "eq3",
  "mod2",
  "mod3",
  "delay2",
  "delay3",
  "reverb2",
  "reverb3",
  "split",
  "lane2",
  "mix",
  "amp2",
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

/* ─────────────────────────────── block kinds ───────────────────────────── */

/** The ten built-in kinds, in enum order — also the picker's row order. `amp2`
 *  joined this list in docs/SPLIT.md §1: an ordinary, freely-placeable
 *  singleton block (engine B), addable through the same generic mechanism as
 *  gate/amp/cab. The fx slots are singleton blocks rather than a kind: which
 *  slot it is carries the identity, and what is in it is named in text. */
export const BLOCK_KINDS = [
  "gate",
  "comp",
  "drive",
  "amp",
  "cab",
  "eq",
  "mod",
  "delay",
  "reverb",
  "amp2",
] as const;

export type BlockKind = (typeof BLOCK_KINDS)[number];

/**
 * The split-path structure's own furniture (docs/SPLIT.md §1): `split`/`mix`
 * are singleton tiles with panels, `lane2` is the hidden lane-B boundary token
 * — it round-trips through parse/pack but is never a tile, never a picker row,
 * never addable through `freeInstance`. Kept OUT of `BLOCK_KINDS` (and so out
 * of the generic picker's `BLOCK_BASES`) on purpose: split/mix only ever enter
 * the chain as the `split, lane2, mix` triple, through the board's dedicated
 * "Add split" affordance, never one at a time.
 */
export const STRUCTURE_BLOCK_IDS = ["split", "lane2", "mix"] as const;

export type StructureBlockId = (typeof STRUCTURE_BLOCK_IDS)[number];

export function isStructureBlockId(value: string): value is StructureBlockId {
  return (STRUCTURE_BLOCK_IDS as readonly string[]).includes(value);
}

/** What identity tables are written per: one entry for each kind, one per fx
 *  slot, one per structural token. Every instance token reads its kind's entry
 *  (see `blockRecord`). */
export type BaseBlockId = BlockKind | FxBlockId | StructureBlockId;

/** Kinds that exist three times over. gate, amp, cab and the fx slots stay
 *  singletons. */
export const DUPLICABLE_KINDS = [
  "comp",
  "drive",
  "eq",
  "mod",
  "delay",
  "reverb",
] as const;

export type DuplicableKind = (typeof DUPLICABLE_KINDS)[number];

/** Instance numbers that mint parameter ids of their own; instance 1 is the
 *  legacy set (`comp_threshold`, not `comp1_threshold`). */
export const EXTRA_INSTANCES = [2, 3] as const;

/** Each kind's instance tokens, instance 1 first. */
export const KIND_INSTANCES = {
  gate: ["gate"],
  comp: ["comp", "comp2", "comp3"],
  drive: ["drive", "drive2", "drive3"],
  amp: ["amp"],
  cab: ["cab"],
  eq: ["eq", "eq2", "eq3"],
  mod: ["mod", "mod2", "mod3"],
  delay: ["delay", "delay2", "delay3"],
  reverb: ["reverb", "reverb2", "reverb3"],
  amp2: ["amp2"],
} as const satisfies Record<BlockKind, readonly BlockId[]>;

const BLOCK_KIND_OF = {} as Record<BlockId, BaseBlockId>;
const BLOCK_INSTANCE_OF = {} as Record<BlockId, number>;

for (const slot of FX_BLOCK_IDS) {
  BLOCK_KIND_OF[slot] = slot;
  BLOCK_INSTANCE_OF[slot] = 0;
}
// Structural furniture is its own singleton kind each, mirroring ChainOrder.h's
// `kindOf` (split/lane2/mix/amp2 share one switch case there, each returning
// itself) — matched here for split/lane2/mix; amp2 goes through the BLOCK_KINDS
// loop below instead, since (unlike these three) it belongs in the generic
// picker.
for (const id of STRUCTURE_BLOCK_IDS) {
  BLOCK_KIND_OF[id] = id;
  BLOCK_INSTANCE_OF[id] = 0;
}
for (const kind of BLOCK_KINDS) {
  const tokens: readonly BlockId[] = KIND_INSTANCES[kind];
  tokens.forEach((id, index) => {
    BLOCK_KIND_OF[id] = kind;
    BLOCK_INSTANCE_OF[id] = index;
  });
}

/** The kind a token is an instance of; an fx slot is its own base. */
export function kindOf(id: BlockId): BaseBlockId {
  return BLOCK_KIND_OF[id];
}

/** 0-based, mirroring `chain::instanceOf`: `comp` is 0, `comp3` is 2. Anything
 *  user-facing shows `instanceOf(id) + 1` ("COMP 3"). */
export function instanceOf(id: BlockId): number {
  return BLOCK_INSTANCE_OF[id];
}

/** A kind's instance tokens, instance 1 first. */
export function instanceTokensOfKind(kind: BlockKind): readonly BlockId[] {
  return KIND_INSTANCES[kind];
}

/** A total table over all 24 tokens from one factory. Identity tables are
 *  written once per base and mapped through this, so an instance can never
 *  drift from its kind's colour, glyph or knob row. */
export function blockRecord<T>(make: (id: BlockId) => T): Record<BlockId, T> {
  const table = {} as Record<BlockId, T>;
  for (const id of BLOCK_IDS) table[id] = make(id);
  return table;
}

/* ─────────────────────────── parameter identifiers ─────────────────────── */

/**
 * 125 slider params — ids are the APVTS ids verbatim and are frozen.
 *
 * The first 29 are v1's, in v1 order. The rest are appended in Parameters.cpp's
 * v2 append order: each duplicable kind's instances 2 and 3 (same ranges,
 * defaults and steps as instance 1, `<kind><n>_<key>`), then the amp's own tone
 * stack, which is eq-shaped but belongs to the amp block and so does not follow
 * the instance rule, then the stereo-chain additions (docs/STEREO.md §4):
 * delay ratio x3, delay width x3, reverb width x3, then the split-path
 * additions (docs/SPLIT.md §2): amp2's own input/output, split's crossover,
 * and mix's five level/pan controls, then the reverb-engine batch
 * (docs/REVERB.md §5, `kVersionHint3`): eleven new sliders x3, then the
 * Shimmer batch (§5, Stage 3, `kVersionHint4`): `reverb_shimmer` x3 — in
 * `WebEditor.cpp`'s `kSliderIds` order, which is not the APVTS layout order
 * (see paramMeta.ts for that one).
 */
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

  "comp2_threshold",
  "comp2_ratio",
  "comp2_attack",
  "comp2_release",
  "comp2_makeup",
  "comp3_threshold",
  "comp3_ratio",
  "comp3_attack",
  "comp3_release",
  "comp3_makeup",
  "drive2_gain",
  "drive2_tone",
  "drive2_level",
  "drive3_gain",
  "drive3_tone",
  "drive3_level",
  "eq2_bass",
  "eq2_mid",
  "eq2_treble",
  "eq3_bass",
  "eq3_mid",
  "eq3_treble",
  "mod2_rate",
  "mod2_depth",
  "mod2_mix",
  "mod3_rate",
  "mod3_depth",
  "mod3_mix",
  "delay2_time",
  "delay2_feedback",
  "delay2_mix",
  "delay3_time",
  "delay3_feedback",
  "delay3_mix",
  "reverb2_size",
  "reverb2_damping",
  "reverb2_mix",
  "reverb3_size",
  "reverb3_damping",
  "reverb3_mix",
  "amp_eq_bass",
  "amp_eq_mid",
  "amp_eq_treble",

  "delay_ratio",
  "delay2_ratio",
  "delay3_ratio",
  "delay_width",
  "delay2_width",
  "delay3_width",
  "reverb_width",
  "reverb2_width",
  "reverb3_width",

  "amp2_input",
  "amp2_output",
  "split_xover",
  "mix_alevel",
  "mix_blevel",
  "mix_apan",
  "mix_bpan",
  "mix_level",

  /* The reverb engine (docs/REVERB.md §5), appended at `kVersionHint3` in the
     spec's parameter order, each id x3 instances — `reverb_algo` is a choice
     and lives in COMBO_PARAM_IDS, and `reverb_size`/`_damping`/`_mix`/`_width`
     keep their frozen ids above and now drive the new engine. Grouped
     id-then-instance like the stereo-chain block, because that is the shape
     `kSliderIds` appends in. */
  "reverb_decay",
  "reverb2_decay",
  "reverb3_decay",
  "reverb_predelay",
  "reverb2_predelay",
  "reverb3_predelay",
  "reverb_diffusion",
  "reverb2_diffusion",
  "reverb3_diffusion",
  "reverb_lowcut",
  "reverb2_lowcut",
  "reverb3_lowcut",
  "reverb_highcut",
  "reverb2_highcut",
  "reverb3_highcut",
  "reverb_mod",
  "reverb2_mod",
  "reverb3_mod",
  "reverb_bassmult",
  "reverb2_bassmult",
  "reverb3_bassmult",
  "reverb_erlevel",
  "reverb2_erlevel",
  "reverb3_erlevel",
  "reverb_color",
  "reverb2_color",
  "reverb3_color",
  "reverb_tilt",
  "reverb2_tilt",
  "reverb3_tilt",
  "reverb_duck",
  "reverb2_duck",
  "reverb3_duck",

  /* Reverb engine batch 2 (docs/REVERB.md §5, Stage 3): Shimmer level. Its
     partner, `reverb_shimmer_interval`, is a choice and lives in
     COMBO_PARAM_IDS below. */
  "reverb_shimmer",
  "reverb2_shimmer",
  "reverb3_shimmer",
] as const;

/**
 * 30 toggle params: v1's 13, then every instance's bypass and the amp EQ's,
 * then the split-path enables (docs/SPLIT.md §2). `amp_stereo` is gone —
 * SUPERSEDED by `amp2` as a chain block (docs/SPLIT.md, top): there is no
 * toggle for it any more, only the block's own `amp2_on`.
 */
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

  "comp2_on",
  "comp3_on",
  "drive2_on",
  "drive3_on",
  "eq2_on",
  "eq3_on",
  "mod2_on",
  "mod3_on",
  "delay2_on",
  "delay3_on",
  "reverb2_on",
  "reverb3_on",
  "amp_eq_on",

  "amp2_on",
  "split_on",
  "mix_on",
  "mix_bphase",
] as const;

/** 14 combo params — mod, delay (docs/STEREO.md §4) and now reverb
 *  (docs/REVERB.md §5) are the duplicable kinds with a choice parameter;
 *  `split_mode` (docs/SPLIT.md §2) is the split region's, FROZEN once shipped
 *  like `delay_mode`. `reverb_algo`'s six choices are frozen and declared in
 *  full from day one (§4) so the choice normalisation divisor never moves under
 *  a recorded automation lane. `reverb_shimmer_interval` (Stage 3, §3.11) is
 *  its own FROZEN-4 combo, inert outside Shimmer like `reverb_bassmult` is
 *  inert in Reverse (§5.2). */
export const COMBO_PARAM_IDS = [
  "amp_out_mode",
  "mod_type",
  "mod2_type",
  "mod3_type",

  "delay_mode",
  "delay2_mode",
  "delay3_mode",

  "split_mode",

  "reverb_algo",
  "reverb2_algo",
  "reverb3_algo",

  /* Reverb engine batch 2 (docs/REVERB.md §5, Stage 3): Shimmer's interval,
     FROZEN at four choices. */
  "reverb_shimmer_interval",
  "reverb2_shimmer_interval",
  "reverb3_shimmer_interval",
] as const;

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
  /** Row lengths partitioning `chainOrder` across the board's rows. `[]` = auto:
   *  C++ keeps no layout it could not validate, and the UI wraps for itself. */
  chainRows: number[];
  model: ModelInfo | null;
  /** Engine B's loaded capture (docs/STEREO.md §1 plumbing, now driven by the
   *  `amp2` chain block — docs/SPLIT.md §1); null when empty, same shape as
   *  `model`. Only meaningful while `amp2` is in the chain, but tracked
   *  regardless — clearing model A must not clear B. */
  modelB: ModelInfo | null;
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
  chainChanged: { chainOrder: string[]; chainRows: number[] };
  libraryChanged: { models: FileEntry[]; irs: FileEntry[] };
  presetChanged: {
    presets: PresetInfo[];
    currentPresetName: string;
    ab: AbState;
  };
  modelChanged: { model: ModelInfo | null };
  /** Mirrors `modelChanged` for engine B — `WebEditor::pollModelAndIr` polls
   *  `loadedModelPathB` alongside the A/IR paths it already watches. */
  modelBChanged: { modelB: ModelInfo | null };
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
  /** Order and row lengths travel together — C++ stores them as one consistent
   *  pair. `rows` that do not partition `tokens` are stored as `[]` (auto). */
  setChainOrder(tokens: string[], rows: number[]): Promise<void>;
  loadModel(path: string): Promise<ErrorResult>;
  clearModel(): Promise<void>;
  /** Mirrors `loadModel`/`clearModel` for engine B (docs/STEREO.md §1 plumbing;
   *  the model-B UI now lives on the `amp2` block, docs/SPLIT.md §5). Loading
   *  A's own path here is exactly how "Use model A" works — there is no
   *  separate native fn for it. */
  loadModelB(path: string): Promise<ErrorResult>;
  clearModelB(): Promise<void>;
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
