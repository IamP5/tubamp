/**
 * Drive editor (docs/FX-ENGINES.md §4.3). Instance-aware: each drive block
 * edits its own `<token>_type` and `<token>_*` knobs. Picking a type moves all
 * three knobs to that type's starting points; the knobs are always gain / tone
 * / level, named per type (Drive, Sustain, High cut, …).
 *
 * Display: the waveshaper's transfer curve (its shape follows the type and
 * tightens with the gain) beside the tone control's response, whose corner can
 * be dragged.
 */
import type { BlockId, ComboParamId, SliderParamId } from "../../bridge";
import { useComboParam } from "../../hooks";
import { paramLabel } from "../../theme/words";
import { PlotSplit } from "./display";
import { TonePlot, TransferPlot } from "./driveDisplay";
import { DRIVE_LABELS } from "./driveLabels";
import { DRIVE_STARTING_POINTS, applyStartingPoints } from "./typeDefaults";
import { Editor, HeaderSlot, K, Row, TypeStepper } from "./ui";

/** DRIVE_LABELS' short forms that read better spelled out in a large cell. */
const SPELLED: Readonly<Record<string, string>> = {
  DIST: "Distortion",
  FREQ: "Frequency",
};

const word = (raw: string): string => SPELLED[raw] ?? paramLabel(raw);

export function DriveBody({ block = "drive" }: { block?: BlockId }) {
  const id = (key: string): SliderParamId => `${block}_${key}` as SliderParamId;
  const type = useComboParam(`${block}_type` as ComboParamId);
  const [gain, tone, level] = (DRIVE_LABELS[type.index] ?? DRIVE_LABELS[0]!).map(word);

  return (
    <>
      <HeaderSlot>
        <TypeStepper
          id={`${block}_type` as ComboParamId}
          label="Type"
          onPick={(index) => applyStartingPoints(block, DRIVE_STARTING_POINTS, index)}
        />
      </HeaderSlot>
      <Editor
        display={
          <>
            <TransferPlot block={block} type={type.index} />
            <PlotSplit />
            <TonePlot block={block} type={type.index} label={tone!} />
          </>
        }
      >
        <Row>
          <K id={id("gain")} label={gain} size="lg" />
          <K id={id("tone")} label={tone} size="lg" />
          <K id={id("level")} label={level} size="lg" />
        </Row>
      </Editor>
    </>
  );
}
