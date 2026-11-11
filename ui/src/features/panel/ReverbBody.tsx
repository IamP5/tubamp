/**
 * Reverb body — the TYPE menu (docs/REVERB-DELAY.md §1.2) above one nine-knob
 * row. Instance-aware: each reverb block edits its own `<token>_type` and
 * `<token>_*` knobs. Picking a type moves DECAY and DAMP to that type's starting
 * points (§2.4).
 */
import type { BlockId, ComboParamId } from "../../bridge";
import { KnobRow } from "./KnobRow";
import { KNOB_SPECS } from "./knobSpecs";
import { ParamMenu } from "./ParamMenu";
import { REVERB_STARTING_POINTS, applyStartingPoints } from "./typeDefaults";
import s from "./panel.module.css";

export function ReverbBody({ block = "reverb" }: { block?: BlockId }) {
  return (
    <div className={s.stackBody}>
      <section className={s.centeredSection}>
        <ParamMenu
          id={`${block}_type` as ComboParamId}
          label="Type"
          width={180}
          onPick={(index) => applyStartingPoints(block, REVERB_STARTING_POINTS, index)}
        />
      </section>
      <KnobRow specs={KNOB_SPECS[block]} snug />
    </div>
  );
}
