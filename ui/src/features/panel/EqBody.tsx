/**
 * EQ body — the TYPE menu (docs/FX-ENGINES.md §4.4) above the controls that type
 * reads. Classic keeps its three knobs. Parametric lays nine out left to right in
 * signal order, grouped LOW / MID / HIGH / OUT. Stack adds its VOICE menu and
 * LEVEL. Graphic is seven vertical band faders (100 Hz … 6.4 kHz) and LEVEL.
 * Instance-aware: each EQ block edits its own `<token>_*` ids. Every type is
 * neutral at the frozen defaults, so picking one moves no knobs (no starting
 * points).
 */
import type { BlockId, ComboParamId, SliderParamId } from "../../bridge";
import { useComboParam } from "../../hooks";
import { KnobRow } from "./KnobRow";
import { typedSpecs, type FxTypeSpecName } from "./knobSpecs";
import { ParamFader } from "./ParamFader";
import { ParamMenu } from "./ParamMenu";
import s from "./panel.module.css";

const CLASSIC = 0;
const PARAMETRIC = 1;
const STACK = 2;
const GRAPHIC = 3;

/** Band id suffix and its label, low to high. */
const GRAPHIC_BANDS = [
  ["g100", "100"],
  ["g200", "200"],
  ["g400", "400"],
  ["g800", "800"],
  ["g1k6", "1.6k"],
  ["g3k2", "3.2k"],
  ["g6k4", "6.4k"],
] as const;

const PARAMETRIC_GROUPS: readonly (readonly [string, FxTypeSpecName])[] = [
  ["Low", "eqParamLow"],
  ["Mid", "eqParamMid"],
  ["High", "eqParamHigh"],
  ["Out", "eqParamOut"],
];

export function EqBody({ block = "eq" }: { block?: BlockId }) {
  const type = useComboParam(`${block}_type` as ComboParamId);

  return (
    <div className={s.stackBody}>
      <section className={s.menuRow}>
        <ParamMenu id={`${block}_type` as ComboParamId} label="Type" width={130} minWidth={130} />
        {type.index === STACK && (
          <ParamMenu id={`${block}_voice` as ComboParamId} label="Voice" width={130} minWidth={130} />
        )}
      </section>

      {type.index === CLASSIC && <KnobRow specs={typedSpecs(block, "eqClassic")} />}

      {type.index === PARAMETRIC && (
        <div className={s.knobGroups}>
          {PARAMETRIC_GROUPS.map(([caption, name], i) => (
            <div key={name} className={s.knobGroupWrap}>
              {i > 0 && <span className={s.groupDivider} />}
              <div className={s.knobGroup}>
                <span className={s.groupCaption}>{caption}</span>
                <KnobRow specs={typedSpecs(block, name)} snug />
              </div>
            </div>
          ))}
        </div>
      )}

      {type.index === STACK && <KnobRow specs={typedSpecs(block, "eqStack")} />}

      {type.index === GRAPHIC && (
        <div className={s.knobGroups}>
          <div className={s.faderBank}>
            {GRAPHIC_BANDS.map(([key, label]) => (
              <ParamFader key={key} id={`${block}_${key}` as SliderParamId} label={label} />
            ))}
          </div>
          <span className={s.groupDivider} />
          <KnobRow specs={typedSpecs(block, "eqGraphicLevel")} />
        </div>
      )}
    </div>
  );
}
