/**
 * The reverb body's mode row: kit `Segmented` bound to `<block>_algo` (all six
 * frozen choices, one click = `algo.setIndex(i)` — the identical call ParamMenu
 * makes; a mode change NEVER writes any other parameter), plus the INTERVAL
 * `ParamMenu` in the row's right slot, rendered only in Shimmer. The right
 * slot is dead space in all six modes, so its appearance moves nothing below.
 *
 * The `data-rv-modebar` wrapper powers segment-hover mode previews in
 * `useReverbInfo` (delegation — kit Segmented gets no new props), and stamps
 * `data-param-index` so Logic's touch-to-select still reaches `reverb_algo`.
 */
import { Segmented } from "../../components";
import type { BlockId, ComboParamId } from "../../bridge";
import { REVERB_ALGO_CHOICES } from "../../bridge/paramMeta";
import type { ComboParam } from "../../hooks";
import { ParamMenu } from "./ParamMenu";
import { REVERB_ALGO } from "./knobSpecs";
import s from "./panel.module.css";

export interface ReverbModeBarProps {
  block: BlockId;
  algo: ComboParam;
}

export function ReverbModeBar({ block, algo }: ReverbModeBarProps) {
  /* `choices` arrives with the async properties push; fall back to the frozen
     list so the bar (the panel's primary control) never renders empty. */
  const labels = algo.choices.length > 0 ? algo.choices : [...REVERB_ALGO_CHOICES];
  const options = labels.map((label, index) => ({
    value: String(index),
    label,
  }));

  return (
    <div className={s.rvModeRow}>
      <div
        className={s.rvModeBar}
        data-rv-modebar=""
        data-param-index={algo.parameterIndex}
      >
        <Segmented
          options={options}
          value={String(algo.index)}
          onChange={(value) => algo.setIndex(Number(value))}
          aria-label={algo.name || "Reverb algorithm"}
        />
      </div>
      {algo.index === REVERB_ALGO.shimmer && (
        <div className={s.rvIntervalSlot} data-rv-key="shimmer_interval">
          <ParamMenu
            id={`${block}_shimmer_interval` as ComboParamId}
            label="Interval"
            width={168}
          />
        </div>
      )}
    </div>
  );
}
