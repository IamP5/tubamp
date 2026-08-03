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
 */
import { useEffect, useState } from "react";
import {
  Button,
  DownloadIcon,
  Knob,
  PlusIcon,
  Spinner,
  Toggle,
  Tooltip,
} from "../../components";
import { bridge } from "../../bridge";
import { useToggleParam } from "../../hooks";
import { useStore } from "../../store";
import { FileRow } from "./FileRow";
import { ParamMenu } from "./ParamMenu";
import { ParamSlider } from "./ParamSlider";
import { formatSampleRate, isEngineLive } from "./library";
import s from "./panel.module.css";

const NO_MODEL = "No model — load a NAM capture";
const T3K_HINT = "Add your TONE3000 publishable key to browse captures.";

export function AmpBody() {
  const model = useStore((st) => st.model);
  const models = useStore((st) => st.models);
  const configured = useStore((st) => st.t3k.configured);
  const downloads = useStore((st) => st.downloads);
  const tone = useStore((st) => st.tone);
  const loadModel = useStore((st) => st.loadModel);
  const clearModel = useStore((st) => st.clearModel);
  const importModel = useStore((st) => st.importModel);
  const t3kConfigure = useStore((st) => st.t3kConfigure);
  const t3kSignOut = useStore((st) => st.t3kSignOut);
  const t3kStartSelectFlow = useStore((st) => st.t3kStartSelectFlow);
  const toast = useStore((st) => st.toast);

  const calInput = useToggleParam("amp_cal_input");

  /* The select flow is a browser round-trip: keep the button honest while it
     runs. Resolution arrives as an event, never as the call's return value, and
     either outcome (a tone, or an error/cancel) ends the wait. */
  const [browsing, setBrowsing] = useState(false);
  useEffect(() => {
    const offTone = bridge.on("t3kToneSelected", () => setBrowsing(false));
    const offError = bridge.on("t3kError", () => setBrowsing(false));
    return () => {
      offTone();
      offError();
    };
  }, []);

  const live = isEngineLive(model);
  const active = Object.entries(downloads);

  return (
    <div className={s.ampBody}>
      <section className={s.column}>
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
              <Button
                size="sm"
                variant="primary"
                icon={
                  browsing ? <Spinner size={14} /> : <DownloadIcon size={14} />
                }
                disabled={browsing}
                onClick={() => {
                  setBrowsing(true);
                  void t3kStartSelectFlow();
                }}
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
              const name =
                tone?.models.find((m) => String(m.id) === id)?.name ??
                `Model ${id}`;
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

      <section className={s.column}>
        <span className={s.caption}>Levels</span>
        <div className={s.levels}>
          <div className={s.knobRow}>
            <div className={s.knobCell}>
              <Knob id="amp_input" label="IN" />
            </div>
            <div className={s.knobCell}>
              <Knob id="amp_output" label="OUT" />
            </div>
            <div className={s.knobCell}>
              <Knob id="amp_cal_level" label="CAL LEVEL" size="sm" />
            </div>
          </div>

          <div className={s.fieldStack}>
            <ParamMenu id="amp_out_mode" label="Mode" width={132} />
            <div className={s.field} data-param-index={calInput.parameterIndex}>
              <span className={s.fieldLabel}>Cal in</span>
              <Toggle
                checked={calInput.value}
                onChange={calInput.setValue}
                aria-label="Calibrate input"
              />
            </div>
            {model?.isSlimmable && <ParamSlider id="amp_slim" label="Slim" />}
          </div>
        </div>
      </section>
    </div>
  );
}
