/**
 * Delay editor (docs/REVERB-DELAY.md §1.1, §3; docs/STEREO.md §2).
 * Instance-aware: each delay block edits its own `<token>_type|mode|sync` and
 * `<token>_*` knobs.
 *
 * Picking a type moves High cut, Color and Duck to that type's starting points
 * (§3.4). A note-value sync replaces Time, which dims and shows the note value.
 * Ratio only acts in Dual mode (STEREO.md §2) and dims in the others.
 *
 * Display: the repeats on a time axis (left above, right below) beside the
 * repeats' low/high cut. Time, Feedback and both cuts can be dragged there.
 */
import type { BlockId, ComboParamId, SliderParamId } from "../../bridge";
import { useComboParam } from "../../hooks";
import { CutPlot, PlotSplit } from "./display";
import { TapPlot } from "./delayDisplay";
import { DELAY_STARTING_POINTS, applyStartingPoints } from "./typeDefaults";
import { ChoiceMenu, ChoiceSegment, Editor, Group, HeaderSlot, K, Row, TypeStepper } from "./ui";

const SYNC_OFF = 0;
const MODE_DUAL = 2;

export function DelayBody({ block = "delay" }: { block?: BlockId }) {
  const id = (key: string): SliderParamId => `${block}_${key}` as SliderParamId;
  const sync = useComboParam(`${block}_sync` as ComboParamId);
  const mode = useComboParam(`${block}_mode` as ComboParamId);
  const synced = sync.index !== SYNC_OFF;

  return (
    <>
      <HeaderSlot>
        <TypeStepper
          id={`${block}_type` as ComboParamId}
          label="Type"
          onPick={(index) => applyStartingPoints(block, DELAY_STARTING_POINTS, index)}
        />
        <ChoiceSegment id={`${block}_mode` as ComboParamId} label="Mode" />
        <ChoiceMenu id={`${block}_sync` as ComboParamId} label="Sync" width={80} />
      </HeaderSlot>
      <Editor
        display={
          <>
            <TapPlot block={block} />
            <PlotSplit />
            <CutPlot lowCut={id("lowcut")} highCut={id("highcut")} flex={0.6} label="Delay filter" />
          </>
        }
      >
        <Row>
          <K
            id={id("time")}
            label="Time"
            size="lg"
            disabled={synced}
            valueText={synced ? sync.text : undefined}
          />
          <K id={id("feedback")} label="Feedback" size="lg" />
          <K id={id("mix")} label="Mix" size="lg" />
        </Row>
        {/* One group: the band at 780 (TONE3000 drawer open) has no room for
            the space between three. */}
        <Row>
          <Group>
            <K id={id("duck")} label="Duck" />
            <K id={id("color")} label="Color" />
            <K id={id("mod")} label="Mod" />
            <K id={id("ratio")} label="Ratio" disabled={mode.index !== MODE_DUAL} />
            <K id={id("width")} label="Width" />
            <K id={id("lowcut")} label="Low cut" />
            <K id={id("highcut")} label="High cut" />
          </Group>
        </Row>
      </Editor>
    </>
  );
}
