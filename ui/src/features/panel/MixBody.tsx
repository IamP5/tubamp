/**
 * Mix body — the B-phase toggle (docs/SPLIT.md §2) above the A/B level+pan and
 * master-level knob row, mirroring ModBody/DelayBody/SplitBody's stacked-header
 * shape. Not instance-aware: mix is a singleton, exactly one per chain
 * (docs/SPLIT.md, top).
 */
import { Toggle } from "../../components";
import { useToggleParam } from "../../hooks";
import { KnobRow } from "./KnobRow";
import { KNOB_SPECS } from "./knobSpecs";
import s from "./panel.module.css";

export function MixBody() {
  const phase = useToggleParam("mix_bphase");

  return (
    <div className={s.stackBody}>
      <section className={s.centeredSection}>
        <div className={s.field} data-param-index={phase.parameterIndex}>
          <span className={s.fieldLabel}>B Phase</span>
          <Toggle
            checked={phase.value}
            onChange={phase.setValue}
            aria-label="Invert lane B before summing"
          />
        </div>
      </section>
      <KnobRow specs={KNOB_SPECS.mix} />
    </div>
  );
}
