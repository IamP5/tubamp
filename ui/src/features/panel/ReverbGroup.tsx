/**
 * One captioned tier-2 column of the reverb body: a hairlined caption (with an
 * optional per-mode note) over four compact `ParamSlider` rows. Rows never
 * scroll in practice — `.rvGroupRows`'s overflow is a backstop, not a design.
 *
 * Instance-aware like everything else in ReverbBody: the group's rows are BASE
 * ids mapped through `forInstance(block, id)` at render time. Greyed rows
 * (Reverse's EARLY/BASS/DAMP) stay exactly in place — opacity only, the value
 * keeps moving underneath — and still drive the hover strip, because the
 * `data-rv-key` wrapper is not `pointer-events: none` even when the control is.
 */
import type { BlockId } from "../../bridge";
import { ParamSlider } from "./ParamSlider";
import {
  forInstance,
  REVERB_ALGO,
  reverbGroupRowLabel,
  type ReverbGroup as ReverbGroupSpec,
} from "./knobSpecs";
import s from "./panel.module.css";

export interface ReverbGroupProps {
  block: BlockId;
  algo: number;
  group: ReverbGroupSpec;
}

export function ReverbGroup({ block, algo, group }: ReverbGroupProps) {
  const note = group.note?.[algo];
  return (
    <div className={s.rvGroup}>
      <div className={s.rvCaption} data-rv-key={group.infoKey}>
        <span>{group.caption}</span>
        {note !== undefined && <span className={s.rvCaptionNote}>{note}</span>}
      </div>
      <div className={s.rvGroupRows}>
        {group.rows.map((row) => (
          <div
            key={row.id}
            className={s.rvRow}
            data-rv-key={row.id.slice("reverb_".length)}
          >
            <ParamSlider
              id={forInstance(block, row.id)}
              label={reverbGroupRowLabel(row, algo)}
              disabled={algo === REVERB_ALGO.reverse && row.inertInReverse === true}
            />
          </div>
        ))}
      </div>
    </div>
  );
}
