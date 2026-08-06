/**
 * Amp 2 body — engine B's model management, moved out of AmpBody's old STEREO
 * section now that amp2 is its own chain block (docs/SPLIT.md §5): the same
 * library picker, "Use model A" shortcut and clear affordance, plus the
 * block's own IN/OUT knobs. amp2's own bypass (`amp2_on`) is the panel
 * header's power pill, like every other block — no toggle for it here.
 *
 * Deliberately thinner than AmpBody: no TONE3000 browse/import actions and no
 * tone stack — engine B has neither (docs/SPLIT.md §3 notes users add an EQ
 * instance instead) — and no calibration/output-mode fields, which stay
 * amp-only.
 */
import { Button, Knob, Tooltip } from "../../components";
import { useStore } from "../../store";
import { FileRow } from "./FileRow";
import { formatSampleRate, isEngineLive, rigLabel } from "./library";
import s from "./panel.module.css";

const NO_MODEL_B = "No model — load a NAM capture";
const RIG_TIP =
  "This capture was taken through a cabinet — the Cab block would stack a second one on top of it.";

export function Amp2Body() {
  const model = useStore((st) => st.model);
  const modelB = useStore((st) => st.modelB);
  const models = useStore((st) => st.models);
  const loadModelB = useStore((st) => st.loadModelB);
  const clearModelB = useStore((st) => st.clearModelB);
  const toast = useStore((st) => st.toast);

  const live = isEngineLive(modelB);
  const rig = rigLabel(modelB);

  return (
    <div className={s.ampBody}>
      <section className={s.columnModel}>
        <div className={s.captionRow}>
          <span className={s.caption}>Model</span>
        </div>

        <div className={s.modelLine}>
          <span
            className={modelB ? s.modelName : s.modelNameEmpty}
            title={modelB?.path ?? NO_MODEL_B}
          >
            {modelB ? modelB.name : NO_MODEL_B}
          </span>
          {modelB && (
            <>
              <span className={live ? s.chipLive : s.chipDown}>
                {live ? "LIVE" : "NOT RUNNING"}
              </span>
              {rig && (
                <Tooltip label={RIG_TIP} placement="top">
                  <span className={s.chipRig}>{rig}</span>
                </Tooltip>
              )}
              {live && (
                <span className={s.meta}>
                  {formatSampleRate(modelB.sampleRateHz)}
                </span>
              )}
            </>
          )}
        </div>

        <FileRow
          entries={models}
          currentPath={modelB?.path ?? null}
          placeholder="Select a model…"
          emptyLabel="No models in the library"
          onSelect={(path) => void loadModelB(path)}
          onClear={() => void clearModelB()}
          onEmptyStep={() =>
            toast("No NAM models in the library yet — import one.", "warning")
          }
          labels={{
            prev: "Previous model",
            next: "Next model",
            clear: "Clear the loaded model",
          }}
          trailing={
            <Tooltip label="Load model A into engine B">
              <Button
                size="sm"
                disabled={!model}
                onClick={() => model && void loadModelB(model.path)}
              >
                Use model A
              </Button>
            </Tooltip>
          }
        />
      </section>

      <div className={s.divider} />

      <section className={s.columnGroup}>
        <span className={s.caption}>Levels</span>
        <div className={s.knobRow}>
          <div className={s.knobCellSnug}>
            <Knob id="amp2_input" label="IN" />
          </div>
          <div className={s.knobCellSnug}>
            <Knob id="amp2_output" label="OUT" />
          </div>
        </div>
      </section>
    </div>
  );
}
