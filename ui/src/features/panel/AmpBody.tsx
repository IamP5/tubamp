/**
 * Amp body — NAM model management + the level controls (the React port of
 * `AmpBody`, current-ui-inventory.md §AmpBody).
 *
 * Differences from the native panel, all deliberate:
 * - No 4 Hz status timer. Model name, engine status and slim availability are
 *   derived from `store.model`, which the `modelChanged` event keeps fresh.
 * - No `updatingCombo` reentrancy flag: the list is derived state.
 * - TONE3000 download state lives in the store, not in this component, so a
 *   download survives switching to another block (the native code kept AmpBody
 *   alive forever for exactly this reason).
 * - Errors surface as toasts instead of the dual-purpose hint label.
 *
 * No STEREO section: dual-NAM stereo (docs/STEREO.md §1) is superseded by
 * `amp2` as its own chain block (docs/SPLIT.md, top) — engine B's model
 * management now lives in Amp2Body.
 */
import {
  Button,
  DownloadIcon,
  Knob,
  PlusIcon,
  Toggle,
  Tooltip,
} from "../../components";
import { useToggleParam } from "../../hooks";
import { useStore } from "../../store";
import { FileRow } from "./FileRow";
import { ParamMenu } from "./ParamMenu";
import { ParamSlider } from "./ParamSlider";
import { formatSampleRate, isEngineLive, rigLabel } from "./library";
import s from "./panel.module.css";

const NO_MODEL = "No model — load a NAM capture";
const T3K_HINT = "Add your TONE3000 publishable key to browse captures.";
const RIG_TIP =
  "This capture was taken through a cabinet — the Cab block would stack a second one on top of it.";

export function AmpBody() {
  const model = useStore((st) => st.model);
  const models = useStore((st) => st.models);
  const configured = useStore((st) => st.t3k.configured);
  const downloads = useStore((st) => st.downloads);
  const pending = useStore((st) => st.t3kPending);
  const loadModel = useStore((st) => st.loadModel);
  const clearModel = useStore((st) => st.clearModel);
  const importModel = useStore((st) => st.importModel);
  const t3kConfigure = useStore((st) => st.t3kConfigure);
  const t3kSignOut = useStore((st) => st.t3kSignOut);
  const openT3kBrowser = useStore((st) => st.openT3kBrowser);
  const toast = useStore((st) => st.toast);

  const calInput = useToggleParam("amp_cal_input");
  const eqOn = useToggleParam("amp_eq_on");

  const live = isEngineLive(model);
  const rig = rigLabel(model);
  // IR downloads share the same progress map; they belong to the cab, not here.
  const active = Object.entries(downloads).filter(
    ([id]) => pending[Number(id)]?.kind !== "wav",
  );

  return (
    <div className={s.ampBody}>
      <section className={s.columnModel}>
        <div className={s.captionRow}>
          <span className={s.caption}>Model</span>
          <span className={s.attribution}>Powered by TONE3000</span>
        </div>

        <div className={s.modelLine}>
          <span
            className={model ? s.modelName : s.modelNameEmpty}
            title={model?.path ?? NO_MODEL}
          >
            {model ? model.name : NO_MODEL}
          </span>
          {model && (
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
                  {formatSampleRate(model.sampleRateHz)}
                </span>
              )}
            </>
          )}
        </div>

        <FileRow
          entries={models}
          currentPath={model?.path ?? null}
          placeholder="Select a model…"
          emptyLabel="No models in the library"
          onSelect={(path) => void loadModel(path)}
          onClear={() => void clearModel()}
          onEmptyStep={() =>
            toast("No NAM models in the library yet — import one.", "warning")
          }
          labels={{
            prev: "Previous model",
            next: "Next model",
            clear: "Clear the loaded model",
          }}
          trailing={
            <Tooltip label="Import a NAM capture (.nam)">
              <Button
                size="sm"
                icon={<PlusIcon size={14} />}
                onClick={() => void importModel()}
              >
                Import
              </Button>
            </Tooltip>
          }
        />

        <div className={s.actionRow}>
          {configured ? (
            <>
              {/* The catalog is browsed in the drawer now, not in the system
                  browser's select flow. */}
              <Button
                size="sm"
                variant="primary"
                icon={<DownloadIcon size={14} />}
                onClick={() => openT3kBrowser("models")}
              >
                Browse TONE3000
              </Button>
              <Button
                size="sm"
                variant="ghost"
                onClick={() => void t3kSignOut()}
              >
                Sign out
              </Button>
            </>
          ) : (
            <>
              <Button size="sm" onClick={() => void t3kConfigure()}>
                Configure TONE3000
              </Button>
              <span className={s.hint}>{T3K_HINT}</span>
            </>
          )}
        </div>

        {active.length > 0 && (
          <div className={s.downloadStrip}>
            {active.map(([id, progress]) => {
              const name = pending[Number(id)]?.name ?? `Model ${id}`;
              return (
                <div key={id} className={s.downloadRow}>
                  <span className={s.downloadName} title={name}>
                    {name}
                  </span>
                  <div className={s.progressTrack}>
                    <div
                      className={s.progressFill}
                      style={{ width: `${Math.round(progress * 100)}%` }}
                    />
                  </div>
                  <span className={s.readout}>
                    {Math.round(progress * 100)}%
                  </span>
                </div>
              );
            })}
          </div>
        )}
      </section>

      <div className={s.divider} />

      {/* The Mode / Cal / Slim fields sit UNDER the knobs, not beside them: a
          side stack made this group ~460px wide and the three columns overlapped
          at the minimum window width. */}
      <section className={s.columnGroup}>
        <span className={s.caption}>Levels</span>
        <div className={s.knobRow}>
          <div className={s.knobCellSnug}>
            <Knob id="amp_input" label="IN" />
          </div>
          <div className={s.knobCellSnug}>
            <Knob id="amp_output" label="OUT" />
          </div>
          <div className={s.knobCellSnug}>
            <Knob id="amp_cal_level" label="CAL LEVEL" size="sm" />
          </div>
        </div>

        <div className={s.fieldsRow}>
          <ParamMenu id="amp_out_mode" label="Mode" width={112} />
          <div className={s.field} data-param-index={calInput.parameterIndex}>
            <span className={s.fieldLabel}>Cal in</span>
            <Toggle
              checked={calInput.value}
              onChange={calInput.setValue}
              aria-label="Calibrate input"
            />
          </div>
        </div>

        {model?.isSlimmable && (
          <div className={s.fieldsRow}>
            <ParamSlider id="amp_slim" label="Slim" />
          </div>
        )}
      </section>

      <div className={s.divider} />

      {/* Mirrors the Levels group exactly — caption, knob trio, fields row —
          so the two knob rows land on the same baseline. */}
      <section className={s.columnGroup}>
        <span className={s.caption}>Tone</span>
        <div className={s.knobRow}>
          <div className={s.knobCellSnug}>
            <Knob id="amp_eq_bass" label="BASS" disabled={!eqOn.value} />
          </div>
          <div className={s.knobCellSnug}>
            <Knob id="amp_eq_mid" label="MID" disabled={!eqOn.value} />
          </div>
          <div className={s.knobCellSnug}>
            <Knob id="amp_eq_treble" label="TREBLE" disabled={!eqOn.value} />
          </div>
        </div>
        <div className={s.fieldsRow}>
          <div className={s.field} data-param-index={eqOn.parameterIndex}>
            <span className={s.fieldLabel}>On</span>
            <Toggle
              checked={eqOn.value}
              onChange={eqOn.setValue}
              aria-label="Enable amp EQ"
            />
          </div>
        </div>
      </section>
    </div>
  );
}
