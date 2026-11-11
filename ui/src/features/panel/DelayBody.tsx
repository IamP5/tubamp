/**
 * Delay body — TYPE / MODE / SYNC menus (docs/REVERB-DELAY.md §1.1,
 * docs/STEREO.md §2) above one ten-knob row. Instance-aware: each delay block
 * edits its own `<token>_type|mode|sync` and `<token>_*` knobs.
 *
 * Picking a type moves HI CUT, COLOR and DUCK to that type's starting points
 * (§3.4); a note-value sync replaces TIME, which dims.
 */
import type { BlockId, ComboParamId } from "../../bridge";
import { useComboParam } from "../../hooks";
import { KnobRow } from "./KnobRow";
import { KNOB_SPECS } from "./knobSpecs";
import { ParamMenu } from "./ParamMenu";
import { DELAY_STARTING_POINTS, applyStartingPoints } from "./typeDefaults";
import s from "./panel.module.css";

const SYNC_OFF = 0;
const NO_KEYS: ReadonlySet<string> = new Set();
/** A note-value sync drives the time. */
const SYNCED_KEYS: ReadonlySet<string> = new Set(["time"]);

export function DelayBody({ block = "delay" }: { block?: BlockId }) {
  const sync = useComboParam(`${block}_sync` as ComboParamId);

  return (
    <div className={s.stackBody}>
      <section className={s.menuRow}>
        <ParamMenu
          id={`${block}_type` as ComboParamId}
          label="Type"
          width={110}
          minWidth={110}
          onPick={(index) => applyStartingPoints(block, DELAY_STARTING_POINTS, index)}
        />
        <ParamMenu id={`${block}_mode` as ComboParamId} label="Mode" width={110} minWidth={110} />
        <ParamMenu id={`${block}_sync` as ComboParamId} label="Sync" width={80} minWidth={80} />
      </section>
      <KnobRow
        specs={KNOB_SPECS[block]}
        snug
        disabledKeys={sync.index !== SYNC_OFF ? SYNCED_KEYS : NO_KEYS}
      />
    </div>
  );
}
