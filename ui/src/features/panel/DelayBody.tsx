/**
 * Delay body — the mode combo (Stereo / Ping-Pong / Dual, docs/STEREO.md §2)
 * above the TIME / FDBK / MIX / RATIO / WIDTH knob row, mirroring ModBody's
 * `${token}_type` placement. Instance-aware: each delay block edits its own
 * `<token>_mode` and `<token>_*` knobs.
 */
import type { BlockId, ComboParamId } from "../../bridge";
import { KnobRow } from "./KnobRow";
import { KNOB_SPECS } from "./knobSpecs";
import { ParamMenu } from "./ParamMenu";
import s from "./panel.module.css";

export function DelayBody({ block = "delay" }: { block?: BlockId }) {
  return (
    <div className={s.stackBody}>
      <section className={s.centeredSection}>
        <ParamMenu id={`${block}_mode` as ComboParamId} label="Mode" width={180} />
      </section>
      <KnobRow specs={KNOB_SPECS[block]} />
    </div>
  );
}
