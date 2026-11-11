/**
 * Drive body — the TYPE menu (docs/FX-ENGINES.md §4.3) above three knobs. The
 * knobs are always gain / tone / level; their labels follow the type (GAIN,
 * DRIVE, DIST, SUSTAIN, BOOST …). Instance-aware: each drive block edits its own
 * `<token>_type` and `<token>_*` knobs. Picking a type moves all three to that
 * type's starting points.
 */
import type { BlockId, ComboParamId } from "../../bridge";
import { useComboParam } from "../../hooks";
import { KnobRow } from "./KnobRow";
import { DRIVE_LABELS, KNOB_SPECS } from "./knobSpecs";
import { ParamMenu } from "./ParamMenu";
import { DRIVE_STARTING_POINTS, applyStartingPoints } from "./typeDefaults";
import s from "./panel.module.css";

export function DriveBody({ block = "drive" }: { block?: BlockId }) {
  const type = useComboParam(`${block}_type` as ComboParamId);
  const labels = DRIVE_LABELS[type.index] ?? DRIVE_LABELS[0];
  const specs = KNOB_SPECS[block].map((spec, i) => ({ ...spec, label: labels?.[i] ?? spec.label }));

  return (
    <div className={s.stackBody}>
      <section className={s.centeredSection}>
        <ParamMenu
          id={`${block}_type` as ComboParamId}
          label="Type"
          width={180}
          onPick={(index) => applyStartingPoints(block, DRIVE_STARTING_POINTS, index)}
        />
      </section>
      <KnobRow specs={specs} />
    </div>
  );
}
