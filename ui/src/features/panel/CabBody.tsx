/**
 * Cab editor: the impulse response (the lead), its low / high cut drawn as a
 * response with a handle on each corner, and Low cut, High cut, Mix.
 *
 * When the amp's capture was already taken through a cabinet the card says so
 * once, on its second line: in warn colour while both cabinets are in the path
 * (the header's power button bypasses this one), quiet otherwise. Load and
 * import failures surface as toasts from the store's actions.
 */
import { Button, Tooltip } from "../../components";
import { useRigCab } from "../../hooks";
import { useStore } from "../../store";
import { CutPlot } from "./display";
import { irFacts, titleOf, useAttribution, type MetaFact } from "./MediaMeta";
import { DISPLAY_ROOM, MediaCard, controlsWidth, useBandWidth } from "./media";
import { Editor, K, Row } from "./ui";

const ROW_LG = 3 * 84 + 2 * 16;

export function CabBody() {
  const ir = useStore((st) => st.ir);
  const missing = useStore((st) => st.missingMedia.ir);
  const irs = useStore((st) => st.irs);
  const model = useStore((st) => st.model);
  const loadIr = useStore((st) => st.loadIr);
  const clearIr = useStore((st) => st.clearIr);
  const importIr = useStore((st) => st.importIr);
  const openT3kBrowser = useStore((st) => st.openT3kBrowser);
  const toast = useStore((st) => st.toast);

  const { captureHasCab, doubleCab } = useRigCab();
  const attribution = useAttribution(ir?.tone);
  const [band, bandRef] = useBandWidth();
  const showDisplay = controlsWidth(band) >= ROW_LG + DISPLAY_ROOM;

  const rig: MetaFact | null = captureHasCab
    ? {
        key: "rig",
        warn: doubleCab,
        node: (
          <Tooltip
            label={
              doubleCab
                ? `${model ? titleOf(model) : "The capture"} was taken through a cabinet, so this IR adds a second one.`
                : `${model ? titleOf(model) : "The capture"} was taken through a cabinet.`
            }
          >
            <span>Capture already has a cab</span>
          </Tooltip>
        ),
      }
    : null;

  return (
    <Editor
      lead={
        <MediaCard
          onRoot={bandRef}
          title={ir ? titleOf(ir) : null}
          titleTip={ir ? (ir.tone?.title ? `${ir.tone.title} — ${ir.name}` : ir.path) : undefined}
          emptyTitle="No IR"
          missing={missing}
          facts={[rig, attribution, ...(ir ? irFacts(ir) : [])]}
          entries={irs}
          currentPath={ir?.path ?? null}
          library="IRs"
          emptyLabel="No IRs in the library"
          onSelect={(path) => void loadIr(path)}
          onClear={() => void clearIr()}
          onEmptyStep={() => toast("No IRs in the library yet. Import one.", "warning")}
          labels={{
            menu: "Choose an IR from the library",
            prev: "Previous IR",
            next: "Next IR",
            clear: "Remove IR",
          }}
          onBrowse={() => openT3kBrowser("irs")}
          secondary={
            <Button variant="ghost" onClick={() => void importIr()}>
              Import file
            </Button>
          }
        />
      }
      display={showDisplay ? <CutPlot lowCut="cab_lowcut" highCut="cab_highcut" label="Cab filter" /> : undefined}
    >
      <Row>
        <K id="cab_lowcut" label="Low cut" size="lg" />
        <K id="cab_highcut" label="High cut" size="lg" />
        <K id="cab_mix" label="Mix" size="lg" />
      </Row>
    </Editor>
  );
}
