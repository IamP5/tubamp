/**
 * Gate body — the MODE menu (docs/FX-ENGINES.md §4.1) above the knobs that mode
 * reads, with the gate meter (state lamp, detector level, gain reduction) beside
 * them. Classic reads only THRESH; Tight and Smooth add RELEASE, HOLD, RANGE and
 * SC HPF (24 dB per octave). Picking a mode moves RELEASE, HOLD and SC HPF to that
 * mode's starting points. HOLD is the wait after a note stops, before the gate may close.
 */
import type { BlockId, ComboParamId } from "../../bridge";
import { useComboParam } from "../../hooks";
import { GateMeter } from "./DynMeters";
import { KnobRow } from "./KnobRow";
import { typedSpecs } from "./knobSpecs";
import { ParamMenu } from "./ParamMenu";
import { GATE_STARTING_POINTS, applyStartingPoints } from "./typeDefaults";
import s from "./panel.module.css";

const CLASSIC = 0;

export function GateBody({ block = "gate" }: { block?: BlockId }) {
  const mode = useComboParam(`${block}_mode` as ComboParamId);
  const classic = mode.index === CLASSIC;

  return (
    <div className={s.dynBody}>
      <div className={s.dynMain}>
        <section className={s.centeredSection}>
          <ParamMenu
            id={`${block}_mode` as ComboParamId}
            label="Mode"
            width={140}
            minWidth={140}
            onPick={(index) => applyStartingPoints(block, GATE_STARTING_POINTS, index)}
          />
        </section>
        <KnobRow specs={typedSpecs(block, classic ? "gateClassic" : "gateFull")} snug />
        <span className={s.dynHint}>
          {classic
            ? "Threshold is a mean-square level"
            : "Threshold is the peak level of the filtered sidechain, dBFS"}
        </span>
      </div>
      <GateMeter classic={classic} />
    </div>
  );
}
