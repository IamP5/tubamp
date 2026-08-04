/**
 * Mod body — the type combo (Chorus / Phaser / Tremolo) above the RATE / DEPTH /
 * MIX knob row, mirroring the native `modTypeCombo` placement. Instance-aware:
 * each modulation block edits its own `<token>_type` and `<token>_*` knobs.
 */
import type { BlockId, ComboParamId } from "../../bridge";
import { KnobRow } from "./KnobRow";
import { KNOB_SPECS } from "./knobSpecs";
import { ParamMenu } from "./ParamMenu";
import s from "./panel.module.css";

export function ModBody({ block = "mod" }: { block?: BlockId }) {
  return (
    <div className={s.stackBody}>
      <section className={s.centeredSection}>
        <ParamMenu id={`${block}_type` as ComboParamId} label="Type" width={180} />
      </section>
      <KnobRow specs={KNOB_SPECS[block]} />
    </div>
  );
}
