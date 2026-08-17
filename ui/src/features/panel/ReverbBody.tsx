/**
 * Reverb body — three fixed bands inside the fixed 182px dock body
 * (docs/REVERB.md §5; --dock-h stays 248px):
 *
 *   mode row   (28px)  Segmented mode bar + (Shimmer only) INTERVAL menu
 *   main       (flex)  4 primary dials | divider | 3 captioned groups of 4
 *   info strip (18px)  hover explanation of whatever the pointer is on
 *
 * Instance-aware like DelayBody: each reverb block edits its own `<token>_algo`
 * and `<token>_*` ids via `forInstance`, so reverb2/reverb3 reach their own
 * parameters. Mode-aware per docs/REVERB.md §5.2: Reverse renders WINDOW for
 * decay and greys EARLY/BASS/DAMP, Spring renders TANK/DWELL, and the decay
 * readout clamps at each mode's ceiling.
 *
 * The SHIMMER dial is ALWAYS MOUNTED and greyed outside Shimmer (§5.2,
 * amended 2026-08-08): the fourth cell would otherwise be an empty hole in
 * five of six modes, and a greyed dial whose hover strip says "switch the mode
 * to bring this to life" teaches the mode set. Only the INTERVAL menu is
 * conditional — it lives in the transient mode row, not the stable tier-1
 * geometry, so DECAY/MIX/SIZE/SHIMMER never move by a pixel on a mode change.
 *
 * A mode change writes `reverb_algo` and NOTHING else — no per-mode default
 * patches, no mix "protection". Every parameter keeps its value across mode
 * switches by design; do not "fix" this.
 *
 * The root stamps `data-reverb-algo` so theme/tokens.css re-tints the body per
 * mode. The tint never leaves this subtree: header, board tile and connectors
 * stay reverb-cyan (block hue identifies the block, mode hue the mode).
 */
import { useRef } from "react";
import type { BlockId, ComboParamId } from "../../bridge";
import { Knob } from "../../components";
import { useComboParam } from "../../hooks";
import {
  REVERB_ALGO_KEYS,
  REVERB_GROUPS,
  reverbPrimarySpecs,
} from "./knobSpecs";
import { ReverbGroup } from "./ReverbGroup";
import { ReverbInfoStrip } from "./ReverbInfoStrip";
import { ReverbModeBar } from "./ReverbModeBar";
import { useReverbInfo } from "./useReverbInfo";
import s from "./panel.module.css";

export function ReverbBody({ block = "reverb" }: { block?: BlockId }) {
  const algoId = `${block}_algo` as ComboParamId;
  const algo = useComboParam(algoId);

  const rootRef = useRef<HTMLDivElement>(null);
  const keyRef = useRef<HTMLSpanElement>(null);
  const textRef = useRef<HTMLSpanElement>(null);
  const rangeRef = useRef<HTMLSpanElement>(null);
  useReverbInfo(rootRef, keyRef, textRef, rangeRef, block, algo.index);

  return (
    <div
      ref={rootRef}
      className={s.rvBody}
      data-reverb-algo={REVERB_ALGO_KEYS[algo.index] ?? REVERB_ALGO_KEYS[0]}
    >
      <ReverbModeBar block={block} algo={algo} />
      <div className={s.rvMain}>
        <div className={s.rvPrimary}>
          {reverbPrimarySpecs(block, algo.index).map((spec) => (
            <div
              key={spec.id}
              className={s.rvKnobCell}
              /* base key: "reverb2_decay" → "decay" */
              data-rv-key={spec.id.slice(spec.id.indexOf("_") + 1)}
            >
              <Knob
                id={spec.id}
                label={spec.label}
                size="lg"
                displayMax={spec.displayMax}
                disabled={spec.disabled}
              />
            </div>
          ))}
        </div>
        <div className={s.divider} />
        <div className={s.rvGroups}>
          {REVERB_GROUPS.map((group) => (
            <ReverbGroup
              key={group.infoKey}
              block={block}
              algo={algo.index}
              group={group}
            />
          ))}
        </div>
      </div>
      <ReverbInfoStrip keyRef={keyRef} textRef={textRef} rangeRef={rangeRef} />
    </div>
  );
}
