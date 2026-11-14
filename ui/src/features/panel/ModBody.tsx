/**
 * Modulation editor (docs/FX-ENGINES.md §2.7.6, §4.5–4.7): the type (Chorus /
 * Phaser / Tremolo), the current type's voicing (mode, model or voice — one
 * shown at a time) and Sync in the header; the phaser's colour switch under its
 * knobs. Instance-aware: each modulation block edits its own `<token>_*` ids.
 *
 * The knobs are every knob some voicing of the current type reads (phaser 3,
 * chorus 5, tremolo 7); what the current voicing ignores dims in place
 * (§4.5.2, §4.6.2, §4.7.2). A note-value sync replaces Rate, which dims and
 * shows the note value; Classic voicings never read sync, so for them Sync dims
 * and Rate stays live.
 *
 * Picking a voicing moves Rate / Depth / Mix / Width to that voicing's starting
 * points; picking a type applies those of the voicing already selected there.
 *
 * Display: the LFO over two cycles, with a marker moving at the rate.
 */
import { bridge, type BlockId, type ComboParamId, type SliderParamId, type ToggleParamId } from "../../bridge";
import { useComboParam, useSliderParam } from "../../hooks";
import { LfoPlot } from "./modDisplay";
import { noteSeconds } from "./tempoSync";
import {
  CHORUS_STARTING_POINTS,
  PHASER_STARTING_POINTS,
  TREM_STARTING_POINTS,
  applyStartingPoints,
} from "./typeDefaults";
import {
  ChoiceMenu,
  Editor,
  Group,
  HeaderSlot,
  K,
  Row,
  SwitchField,
  ToggleSegment,
  TypeStepper,
} from "./ui";

const SYNC_OFF = 0;
const CLASSIC = 0;
const PHASER = 1;
const PHASE_90 = 1;

type ModKind = 0 | 1 | 2; // chorus, phaser, tremolo

interface Voicing {
  /** Id suffix after the block token. */
  key: string;
  menuLabel: string;
  points: Readonly<Record<string, readonly number[]>>;
  /** The small knobs some voicing of this type reads (FX-ENGINES §7.2). */
  small: readonly SmallKey[];
}

type SmallKey = "width" | "tone" | "shape" | "wave";

const SMALL_LABELS: Record<SmallKey, string> = {
  width: "Width",
  tone: "Tone",
  shape: "Shape",
  wave: "Wave",
};

const VOICINGS: readonly Voicing[] = [
  { key: "chorus_mode", menuLabel: "Mode", points: CHORUS_STARTING_POINTS, small: ["width", "tone"] },
  { key: "phaser_model", menuLabel: "Model", points: PHASER_STARTING_POINTS, small: [] },
  { key: "trem_voice", menuLabel: "Voice", points: TREM_STARTING_POINTS, small: ["width", "tone", "shape", "wave"] },
];

const keys = (...k: string[]): ReadonlySet<string> => new Set(k);

/** Knobs the voicing ignores, per mod type, indexed by voicing. */
const IGNORED: readonly (readonly ReadonlySet<string>[])[] = [
  /* chorus: Classic reads neither width nor tone */
  [keys("width", "tone"), keys(), keys(), keys(), keys()],
  /* phaser: Phase 90 and Small Stone run at the pedals' own depth and mix */
  [keys(), keys("depth", "mix"), keys("depth", "mix")],
  /* tremolo: new voices have no dry/wet; each reads its own subset of the rest */
  [
    keys("width", "tone", "shape", "wave"), // Classic
    keys("mix", "tone"), // Smooth
    keys("mix", "tone", "shape", "wave"), // Optical
    keys("mix", "tone", "wave"), // Bias
    keys("mix", "shape", "wave"), // Harmonic
  ],
];

const NO_KEYS: ReadonlySet<string> = keys();

export function ModBody({ block = "mod" }: { block?: BlockId }) {
  const id = (key: string): SliderParamId => `${block}_${key}` as SliderParamId;
  const type = useComboParam(`${block}_type` as ComboParamId);
  const sync = useComboParam(`${block}_sync` as ComboParamId);
  // One hook per voicing id (hooks cannot be conditional); the current type picks.
  const voicings = [
    useComboParam(`${block}_${VOICINGS[0]!.key}` as ComboParamId),
    useComboParam(`${block}_${VOICINGS[1]!.key}` as ComboParamId),
    useComboParam(`${block}_${VOICINGS[2]!.key}` as ComboParamId),
  ];
  const rate = useSliderParam(id("rate"));
  const depth = useSliderParam(id("depth"));
  const width = useSliderParam(id("width"));
  const shape = useSliderParam(id("shape"));
  const wave = useSliderParam(id("wave"));

  const kind = Math.min(2, Math.max(0, type.index)) as ModKind;
  const voicing = VOICINGS[kind]!;
  const voice = voicings[kind]!;
  const classic = voice.index === CLASSIC;
  const synced = !classic && sync.index !== SYNC_OFF;

  const ignored = IGNORED[kind]?.[voice.index] ?? NO_KEYS;
  const off = (key: string): boolean => ignored.has(key);

  // Phaser colour switch: Phase 90's script/block logo, Small Stone's Color.
  const showColor = kind === PHASER && !classic;
  const phase90 = voice.index === PHASE_90;

  const syncedSeconds = synced ? noteSeconds(sync.text) : null;
  const rateHz = syncedSeconds ? 1 / syncedSeconds : rate.scaled;

  return (
    <>
      <HeaderSlot>
        <TypeStepper
          id={`${block}_type` as ComboParamId}
          label="Type"
          onPick={(index) => {
            const next = Math.min(2, Math.max(0, index));
            const current = bridge.comboState(`${block}_${VOICINGS[next]!.key}` as ComboParamId);
            applyStartingPoints(block, VOICINGS[next]!.points, current.getChoiceIndex());
          }}
        />
        <ChoiceMenu
          key={voicing.key}
          id={`${block}_${voicing.key}` as ComboParamId}
          label={voicing.menuLabel}
          width={130}
          onPick={(index) => applyStartingPoints(block, voicing.points, index)}
        />
        <ChoiceMenu id={`${block}_sync` as ComboParamId} label="Sync" width={80} disabled={classic} />
      </HeaderSlot>
      <Editor
        display={
          <LfoPlot
            kind={kind}
            voice={voice.index}
            rateHz={rateHz}
            depth={depth.scaled}
            width={width.scaled}
            shape={shape.scaled}
            wave={wave.scaled}
          />
        }
      >
        <Row>
          <K
            id={id("rate")}
            label="Rate"
            size="lg"
            disabled={synced}
            valueText={synced ? sync.text : undefined}
          />
          <K id={id("depth")} label="Depth" size="lg" disabled={off("depth")} />
          <K id={id("mix")} label="Mix" size="lg" disabled={off("mix")} />
        </Row>
        {/* The phaser's colour switch sits under its three knobs, not in the
            header: with type, model and sync there it would be cut off at
            the 780px band. */}
        {showColor && (
          <Row>
            {phase90 ? (
              <ToggleSegment id={`${block}_color` as ToggleParamId} label="Logo" off="Script" on="Block" />
            ) : (
              <SwitchField id={`${block}_color` as ToggleParamId} label="Color" />
            )}
          </Row>
        )}
        {voicing.small.length > 0 && (
          <Row>
            <Group>
              {voicing.small.map((key) => (
                <K key={key} id={id(key)} label={SMALL_LABELS[key]} disabled={off(key)} />
              ))}
            </Group>
          </Row>
        )}
      </Editor>
    </>
  );
}
