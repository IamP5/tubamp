/**
 * Mod body — the type combo (`mod_type`: Chorus / Phaser / Tremolo) above the
 * RATE / DEPTH / MIX knob row, mirroring the native `modTypeCombo` placement.
 */
import { KnobRow } from "./KnobRow";
import { KNOB_SPECS } from "./knobSpecs";
import { ParamMenu } from "./ParamMenu";
import s from "./panel.module.css";

export function ModBody() {
  return (
    <div className={s.stackBody}>
      <section className={s.centeredSection}>
        <ParamMenu id="mod_type" label="Type" width={180} />
      </section>
      <KnobRow specs={KNOB_SPECS.mod} />
    </div>
  );
}
