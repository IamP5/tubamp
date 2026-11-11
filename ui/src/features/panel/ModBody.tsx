/**
 * Mod body — the TYPE menu (Chorus / Phaser / Tremolo), then the voicing menu of
 * the current type (docs/FX-ENGINES.md §2.7.6: mode, model or voice — one shown at
 * a time), SYNC, and for the phaser the colour switch, above the knobs. Instance-
 * aware: each modulation block edits its own `<token>_*` ids.
 *
 * The knob row shows every knob some voicing of the current type reads and dims
 * what the current voicing ignores (§4.5.2, §4.6.2, §4.7.2). A note-value sync
 * replaces RATE, which dims and shows the note value instead of Hz; Classic
 * voicings never read sync, so for them SYNC dims and RATE stays live.
 *
 * Picking a voicing moves RATE / DEPTH / MIX / WIDTH to that voicing's starting
 * points; picking a type applies those of the voicing already selected there.
 */
import { bridge, type BlockId, type ComboParamId, type ToggleParamId } from "../../bridge";
import { useComboParam } from "../../hooks";
import { KnobRow } from "./KnobRow";
import { typedSpecs, type FxTypeSpecName } from "./knobSpecs";
import { ParamMenu } from "./ParamMenu";
import { ParamSegmentedToggle, ParamToggle } from "./ParamToggle";
import {
  CHORUS_STARTING_POINTS,
  PHASER_STARTING_POINTS,
  TREM_STARTING_POINTS,
  applyStartingPoints,
} from "./typeDefaults";
import s from "./panel.module.css";

const SYNC_OFF = 0;
const CLASSIC = 0;
const PHASE_90 = 1;

type ModKind = 0 | 1 | 2; // chorus, phaser, tremolo

interface Voicing {
  /** Id suffix after the block token. */
  key: string;
  menuLabel: string;
  points: Readonly<Record<string, readonly number[]>>;
  specs: FxTypeSpecName;
}

const VOICINGS: readonly Voicing[] = [
  { key: "chorus_mode", menuLabel: "Mode", points: CHORUS_STARTING_POINTS, specs: "modChorus" },
  { key: "phaser_model", menuLabel: "Model", points: PHASER_STARTING_POINTS, specs: "modPhaser" },
  { key: "trem_voice", menuLabel: "Voice", points: TREM_STARTING_POINTS, specs: "modTrem" },
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
const RATE_KEYS: ReadonlySet<string> = keys("rate");

function union(a: ReadonlySet<string>, b: ReadonlySet<string>): ReadonlySet<string> {
  return b.size === 0 ? a : new Set([...a, ...b]);
}

export function ModBody({ block = "mod" }: { block?: BlockId }) {
  const type = useComboParam(`${block}_type` as ComboParamId);
  const sync = useComboParam(`${block}_sync` as ComboParamId);
  // One hook per voicing id (hooks cannot be conditional); the current type picks.
  const voicings = [
    useComboParam(`${block}_${VOICINGS[0]!.key}` as ComboParamId),
    useComboParam(`${block}_${VOICINGS[1]!.key}` as ComboParamId),
    useComboParam(`${block}_${VOICINGS[2]!.key}` as ComboParamId),
  ];

  const kind = (Math.min(2, Math.max(0, type.index)) as ModKind);
  const voicing = VOICINGS[kind]!;
  const voice = voicings[kind]!;
  const classic = voice.index === CLASSIC;
  const synced = !classic && sync.index !== SYNC_OFF;

  const ignored = IGNORED[kind]?.[voice.index] ?? NO_KEYS;
  const disabledKeys = synced ? union(ignored, RATE_KEYS) : ignored;

  // Phaser colour switch: Phase 90's script/block logo, Small Stone's Color.
  const showColor = kind === 1 && !classic;
  const phase90 = voice.index === PHASE_90;

  return (
    <div className={s.stackBody}>
      <section className={s.menuRow}>
        <ParamMenu
          id={`${block}_type` as ComboParamId}
          label="Type"
          width={110}
          minWidth={110}
          onPick={(index) => {
            const next = Math.min(2, Math.max(0, index));
            const current = bridge.comboState(`${block}_${VOICINGS[next]!.key}` as ComboParamId);
            applyStartingPoints(block, VOICINGS[next]!.points, current.getChoiceIndex());
          }}
        />
        <ParamMenu
          key={voicing.key}
          id={`${block}_${voicing.key}` as ComboParamId}
          label={voicing.menuLabel}
          width={130}
          minWidth={130}
          onPick={(index) => applyStartingPoints(block, voicing.points, index)}
        />
        <div className={classic ? s.fieldDisabled : undefined}>
          <ParamMenu id={`${block}_sync` as ComboParamId} label="Sync" width={80} minWidth={80} />
        </div>
        {showColor && phase90 && (
          <ParamSegmentedToggle id={`${block}_color` as ToggleParamId} label="Logo" off="Script" on="Block" />
        )}
        {showColor && !phase90 && <ParamToggle id={`${block}_color` as ToggleParamId} label="Color" />}
      </section>
      <KnobRow
        specs={typedSpecs(block, voicing.specs)}
        disabledKeys={disabledKeys}
        valueTexts={synced ? { rate: sync.text } : undefined}
      />
    </div>
  );
}
