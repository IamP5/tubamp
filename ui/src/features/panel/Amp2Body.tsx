/**
 * Amp 2 editor: engine B's capture (docs/SPLIT.md §5) and its own Input /
 * Output. Thinner than Amp on purpose: no tone stack (engine B has none,
 * docs/SPLIT.md §3), no calibration or output mode (amp-only). Browse opens
 * the TONE3000 drawer targeted at this engine; "Copy from Amp" loads the Amp
 * block's capture here (`loadModelB(model.path)`), since importing a file
 * installs it into engine A.
 */
import { Button, Tooltip } from "../../components";
import { useStore } from "../../store";
import { rigFact, titleOf, useAttribution } from "./MediaMeta";
import { MediaCard } from "./media";
import m from "./media.module.css";
import { Editor, K, Row } from "./ui";

export function Amp2Body() {
  const model = useStore((st) => st.model);
  const modelB = useStore((st) => st.modelB);
  const missing = useStore((st) => st.missingMedia.modelB);
  const models = useStore((st) => st.models);
  const loadModelB = useStore((st) => st.loadModelB);
  const clearModelB = useStore((st) => st.clearModelB);
  const openT3kBrowser = useStore((st) => st.openT3kBrowser);
  const toast = useStore((st) => st.toast);
  const attribution = useAttribution(modelB?.tone);

  return (
    <Editor
      lead={
        <MediaCard
          title={modelB ? titleOf(modelB) : null}
          titleTip={
            modelB ? (modelB.tone?.title ? `${modelB.tone.title} — ${modelB.name}` : modelB.path) : undefined
          }
          emptyTitle="No capture"
          missing={missing}
          facts={[attribution, rigFact(modelB?.includesCab)]}
          entries={models}
          currentPath={modelB?.path ?? null}
          library="Captures"
          emptyLabel="No captures in the library"
          onSelect={(path) => void loadModelB(path)}
          onClear={() => void clearModelB()}
          onEmptyStep={() => toast("No captures in the library yet. Import one.", "warning")}
          labels={{
            menu: "Choose a capture from the library",
            prev: "Previous capture",
            next: "Next capture",
            clear: "Remove capture",
          }}
          onBrowse={() => openT3kBrowser("models", "amp2")}
          secondary={
            <Tooltip label="Load the Amp block's capture here">
              <Button
                variant="ghost"
                disabled={!model || model.path === modelB?.path}
                onClick={() => model && void loadModelB(model.path)}
              >
                Copy from Amp
              </Button>
            </Tooltip>
          }
        />
      }
    >
      <Row className={m.edge}>
        <K id="amp2_input" label="Input" size="lg" />
        <K id="amp2_output" label="Output" size="lg" />
      </Row>
    </Editor>
  );
}
