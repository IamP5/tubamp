/**
 * Cab editor: the impulse response (the lead: the amp editor's face, with the
 * IR's TONE3000 photo or the default cabinet), its low / high cut drawn as a
 * response with a handle on each corner, and Low cut, High cut, Mix. One
 * component for both cabs: `block` picks the IR slot and the parameter set
 * (store/cabs.ts).
 *
 * When the amp's capture was already taken through a cabinet the face says so
 * once, on its line of facts: in warn colour while both cabinets are in the path
 * (the header's power button bypasses this one), quiet otherwise. The note is
 * the first cab's: it is about the first amp's capture, the one the hook reads.
 * Load and import failures surface as toasts from the store's actions.
 */
import { Tooltip } from "../../components";
import { useRigCab } from "../../hooks";
import type { CabToken } from "../../bridge";
import { CAB_SLOTS, cabSlider, useStore } from "../../store";
import { CutPlot } from "./display";
import { irFacts, titleOf, useAttribution, type MetaFact } from "./MediaMeta";
import { AmpFace } from "./AmpFace";
import { DISPLAY_ROOM, controlsWidth, useBandWidth } from "./media";
import { Editor, K, Row } from "./ui";

const ROW_LG = 3 * 84 + 2 * 16;

export function CabBody({ block = "cab" }: { block?: CabToken }) {
  const slot = CAB_SLOTS[block];
  const ir = useStore(slot.ir);
  const missing = useStore((st) => st.missingMedia[slot.missing]);
  const irs = useStore((st) => st.irs);
  const model = useStore((st) => st.model);
  const loadIr = useStore((st) => st.loadIr);
  const clearIr = useStore((st) => st.clearIr);
  const importIr = useStore((st) => st.importIr);
  const openT3kBrowser = useStore((st) => st.openT3kBrowser);
  const toast = useStore((st) => st.toast);
  const downloads = useStore((st) => st.downloads);
  const pending = useStore((st) => st.t3kPending);

  const { captureHasCab, doubleCab } = useRigCab();
  const attribution = useAttribution(ir?.tone);
  const [band, bandRef] = useBandWidth();
  const showDisplay = controlsWidth(band) >= ROW_LG + DISPLAY_ROOM;

  /* An IR's download shows on the cab it is headed for. */
  const active = Object.entries(downloads)
    .filter(([id]) => {
      const item = pending[Number(id)];
      return item?.kind === "wav" && (item.target ?? "cab") === block;
    })
    .map(([id, progress]) => ({ id, progress, name: pending[Number(id)]?.name ?? `IR ${id}` }));

  const rig: MetaFact | null = captureHasCab && block === "cab"
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
        <AmpFace
          kind="cab"
          onRoot={bandRef}
          title={ir ? titleOf(ir) : null}
          titleTip={ir ? (ir.tone?.title ? `${ir.tone.title} — ${ir.name}` : ir.path) : undefined}
          photo={ir?.tone?.imageUrl ?? null}
          missing={missing}
          facts={[rig, attribution, ...(ir ? irFacts(ir) : [])]}
          entries={irs}
          currentPath={ir?.path ?? null}
          onSelect={(path) => void loadIr(path, block)}
          onClear={() => void clearIr(block)}
          onEmptyStep={() => toast("No IRs in the library yet. Import one.", "warning")}
          sources={[
            { id: "browse", label: "Browse TONE3000", onSelect: () => openT3kBrowser("irs", block) },
            { id: "import", label: "Import file", onSelect: () => void importIr(block) },
          ]}
          downloads={active}
        />
      }
      display={showDisplay ? <CutPlot lowCut={cabSlider(block, "lowcut")} highCut={cabSlider(block, "highcut")} label="Cab filter" /> : undefined}
    >
      <Row>
        <K id={cabSlider(block, "lowcut")} label="Low cut" size="lg" />
        <K id={cabSlider(block, "highcut")} label="High cut" size="lg" />
        <K id={cabSlider(block, "mix")} label="Mix" size="lg" />
      </Row>
    </Editor>
  );
}
