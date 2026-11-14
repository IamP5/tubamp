/**
 * Mixer body (docs/SPLIT.md §2/§5): a two-lane console, then the master.
 *
 *   ┌ A ───────────┐ ┌ B ───── Ø Invert ┐ ┌ MASTER ┐   Center
 *   │ LEVEL   PAN  │ │ LEVEL   PAN      │ │ LEVEL  │   Wide
 *   └──────────────┘ └──────────────────┘ └────────┘   Swap lanes
 *
 * Each lane strip wears its lane's badge and tint (features/board/lanes.ts —
 * the same names the board shows: A/B, L/R or LOW/HIGH by split mode), and
 * its knobs take the lane's colour, so which knob drives which lane is never a
 * matter of reading a label. Lane B's strip holds the phase flip, which only
 * ever applies to lane B. Pans read the console way — C, 40L, 100R — through
 * the Knob primitive's `format`. Quick actions: both pans centred, hard
 * left/right, and swapping what is IN the lanes (one chain write).
 * Not instance-aware: mix is a singleton.
 */
import type { ReactNode } from "react";
import { Button, Knob, Toggle, Tooltip } from "../../components";
import { useSliderParam, useToggleParam } from "../../hooks";
import type { SliderParamId } from "../../bridge";
import {
  LANE_TINT,
  formatPan,
  useLaneNames,
  type LaneSide,
} from "../board/lanes";
import { setLanePans, swapSplitLanes } from "../board/splitActions";
import { SwapGlyph } from "../board/StructureCards";
import l from "./lanes.module.css";

const LANE_PARAMS: Record<LaneSide, { level: SliderParamId; pan: SliderParamId }> = {
  a: { level: "mix_alevel", pan: "mix_apan" },
  b: { level: "mix_blevel", pan: "mix_bpan" },
};

/** Pans within half a percent of a target count as "at" it. */
function near(value: number, target: number): boolean {
  return Math.abs(value - target) < 0.005;
}

export function MixBody() {
  const { names } = useLaneNames();
  const phase = useToggleParam("mix_bphase");
  const aPan = useSliderParam("mix_apan").scaled;
  const bPan = useSliderParam("mix_bpan").scaled;

  const centred = near(aPan, 0) && near(bPan, 0);
  const wide = near(aPan, -1) && near(bPan, 1);

  return (
    <div className={l.mixBody}>
      <LaneStrip side="a" name={names.a} />
      <LaneStrip
        side="b"
        name={names.b}
        extra={
          <Tooltip label={`Invert lane ${names.b}'s polarity before the sum`}>
            <span className={l.phase} data-param-index={phase.parameterIndex}>
              <span className={l.phaseLabel}>Ø Invert</span>
              <Toggle
                checked={phase.value}
                onChange={phase.setValue}
                aria-label={`Invert lane ${names.b} polarity`}
              />
            </span>
          </Tooltip>
        }
      />

      <section className={l.strip} aria-label="Master">
        <header className={l.stripHead}>
          <span className={l.masterTag}>Master</span>
        </header>
        <div className={l.stripKnobs}>
          <div className={l.stripCell}>
            <Knob id="mix_level" label="Level" />
          </div>
        </div>
      </section>

      <div className={l.actions}>
        <Button
          size="sm"
          className={l.action}
          aria-pressed={centred}
          data-active={centred || undefined}
          onClick={() => setLanePans(0, 0)}
        >
          Center
        </Button>
        <Button
          size="sm"
          className={l.action}
          aria-pressed={wide}
          data-active={wide || undefined}
          onClick={() => setLanePans(-1, 1)}
        >
          Wide
        </Button>
        <Button
          size="sm"
          className={l.action}
          icon={<SwapGlyph />}
          onClick={swapSplitLanes}
        >
          Swap lanes
        </Button>
      </div>
    </div>
  );
}

function LaneStrip({
  side,
  name,
  extra,
}: {
  side: LaneSide;
  name: string;
  extra?: ReactNode;
}) {
  const tint = LANE_TINT[side];
  const ids = LANE_PARAMS[side];
  return (
    <section
      className={l.strip}
      data-lane={side}
      aria-label={`Lane ${name}`}
      style={{ boxShadow: `inset 0 2px 0 ${tint.ring}, var(--ring-inset)` }}
    >
      <header className={l.stripHead}>
        <span
          className={l.badge}
          style={{
            color: tint.ink,
            background: tint.fill,
            boxShadow: `inset 0 0 0 1px ${tint.ring}`,
          }}
        >
          <span className={l.badgeDot} style={{ background: tint.ink }} />
          {name}
        </span>
        {extra}
      </header>
      <div className={l.stripKnobs}>
        <div className={l.stripCell}>
          <Knob id={ids.level} label="Level" accent={tint.ink} />
        </div>
        <div className={l.stripCell}>
          <Knob id={ids.pan} label="Pan" accent={tint.ink} format={formatPan} />
        </div>
      </div>
    </section>
  );
}
