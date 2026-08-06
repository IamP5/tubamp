/**
 * Split body — the mode combo (Copy / L/R / X-Over, docs/SPLIT.md §2) above the
 * X-OVER knob, mirroring ModBody/DelayBody's `${token}_mode` placement. Not
 * instance-aware: split is a singleton, exactly one per chain (docs/SPLIT.md,
 * top).
 */
import { KnobRow } from "./KnobRow";
import { KNOB_SPECS } from "./knobSpecs";
import { ParamMenu } from "./ParamMenu";
import s from "./panel.module.css";

export function SplitBody() {
  return (
    <div className={s.stackBody}>
      <section className={s.centeredSection}>
        <ParamMenu id="split_mode" label="Mode" width={180} />
      </section>
      <KnobRow specs={KNOB_SPECS.split} />
    </div>
  );
}
