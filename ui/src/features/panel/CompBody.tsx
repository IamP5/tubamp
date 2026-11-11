/**
 * Compressor body — the TYPE menu (docs/FX-ENGINES.md §4.2) with the AUTO makeup
 * switch and the gain-reduction meter on its row, above the knobs that type
 * reads. Classic keeps its five knobs (AUTO dims: Classic never reads it); the
 * other types add KNEE, MIX and SC HPF. Instance-aware: each compressor block
 * edits its own `<token>_*` knobs and reads its own meter slot. Picking a type
 * moves KNEE, ATTACK and RELEASE to that type's starting points.
 */
import { instanceOf, type BlockId, type ComboParamId, type ToggleParamId } from "../../bridge";
import { useComboParam } from "../../hooks";
import { CompMeter } from "./DynMeters";
import { KnobRow } from "./KnobRow";
import { typedSpecs } from "./knobSpecs";
import { ParamMenu } from "./ParamMenu";
import { ParamToggle } from "./ParamToggle";
import { COMP_STARTING_POINTS, applyStartingPoints } from "./typeDefaults";
import s from "./panel.module.css";

const CLASSIC = 0;
const SQUASH = 2;

export function CompBody({ block = "comp" }: { block?: BlockId }) {
  const type = useComboParam(`${block}_type` as ComboParamId);
  const classic = type.index === CLASSIC;

  return (
    <div className={s.stackBody}>
      <section className={s.dynMenuRow}>
        <ParamMenu
          id={`${block}_type` as ComboParamId}
          label="Type"
          width={110}
          minWidth={110}
          onPick={(index) => applyStartingPoints(block, COMP_STARTING_POINTS, index)}
        />
        <ParamToggle id={`${block}_auto_makeup` as ToggleParamId} label="Auto makeup" disabled={classic} />
        <CompMeter instance={instanceOf(block)} />
      </section>
      <KnobRow specs={typedSpecs(block, classic ? "compClassic" : "compFull")} snug />
      <span className={s.dynHint}>
        {type.index === SQUASH ? "Squash: the knee acts on the output level" : " "}
      </span>
    </div>
  );
}
