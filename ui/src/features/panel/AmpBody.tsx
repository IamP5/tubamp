/**
 * Amp editor: the NAM capture (the lead), the amp's own tone stack drawn as
 * its response, and the level controls (ui/DESIGN.md §Editors).
 *
 *   header   Output mode, Calibrate input
 *   lead     the capture: title, creator, library, Browse TONE3000 / Import
 *   display  the tone stack's response, a handle on each band (when it fits)
 *   rows     Level (Input, Output) and Tone (Bass, Mid, Treble, its switch),
 *            then Cal level and Slim
 *
 * TONE3000 download state lives in the store, so a download survives switching
 * to another block; errors surface as toasts from the store's actions.
 * No tone stack on Amp 2 (docs/SPLIT.md §3): it is this block's alone.
 */
import { Button, cx } from "../../components";
import { useSliderParam, useToggleParam } from "../../hooks";
import { useStore } from "../../store";
import {
  F_MAX,
  F_MIN,
  FreqGrid,
  HRules,
  Plot,
  PlotHandle,
  areaPath,
  bellDb,
  clamp,
  curve,
  dbY,
  freqX,
  highShelfDb,
  lerp,
  linePath,
  lowShelfDb,
  plot as p,
  useEase,
  xFreq,
} from "./display";
import { rigFact, titleOf, useAttribution } from "./MediaMeta";
import { BareSwitch, DISPLAY_ROOM, MediaCard, controlsWidth, useBandWidth } from "./media";
import { ChoiceMenu, Editor, Group, HeaderSlot, K, Row, SwitchField } from "./ui";

/* Row widths at the toolkit's fixed cells (68 small, 84 large, 16px gaps). */
const ROW1_LG = 5 * 84 + 16;
const ROW1_SM = 5 * 68 + 16;

export function AmpBody() {
  const model = useStore((st) => st.model);
  const missing = useStore((st) => st.missingMedia.model);
  const models = useStore((st) => st.models);
  const downloads = useStore((st) => st.downloads);
  const pending = useStore((st) => st.t3kPending);
  const loadModel = useStore((st) => st.loadModel);
  const clearModel = useStore((st) => st.clearModel);
  const importModel = useStore((st) => st.importModel);
  const openT3kBrowser = useStore((st) => st.openT3kBrowser);
  const toast = useStore((st) => st.toast);

  const calInput = useToggleParam("amp_cal_input");
  const eqOn = useToggleParam("amp_eq_on");
  const attribution = useAttribution(model?.tone);
  const [band, bandRef] = useBandWidth();

  /* IR downloads share the progress map; they belong to the cab. */
  const active = Object.entries(downloads)
    .filter(([id]) => pending[Number(id)]?.kind !== "wav")
    .map(([id, progress]) => ({ id, progress, name: pending[Number(id)]?.name ?? `Capture ${id}` }));

  const room = controlsWidth(band);
  const showDisplay = room >= ROW1_LG + DISPLAY_ROOM;
  const layout = room >= ROW1_LG ? "lg" : room >= ROW1_SM ? "sm" : "stack";
  const size = layout === "lg" ? "lg" : "sm";
  const toneOff = !eqOn.value;

  const tone = (
    <Group label="Tone" action={<BareSwitch id="amp_eq_on" label="Enable amp EQ" />}>
      <K id="amp_eq_bass" label="Bass" size={size} disabled={toneOff} />
      <K id="amp_eq_mid" label="Mid" size={size} disabled={toneOff} />
      <K id="amp_eq_treble" label="Treble" size={size} disabled={toneOff} />
    </Group>
  );
  const calLevel = <K id="amp_cal_level" label="Cal level" disabled={!calInput.value} />;
  const slim = <K id="amp_slim" label="Slim" disabled={!model?.isSlimmable} />;

  return (
    <>
      <HeaderSlot>
        <ChoiceMenu id="amp_out_mode" label="Output" width={120} />
        <SwitchField id="amp_cal_input" label="Calibrate input" />
      </HeaderSlot>
      <Editor
        lead={
          <MediaCard
            onRoot={bandRef}
            title={model ? titleOf(model) : null}
            titleTip={model ? (model.tone?.title ? `${model.tone.title} — ${model.name}` : model.path) : undefined}
            emptyTitle="No capture"
            missing={missing}
            facts={[attribution, rigFact(model?.includesCab)]}
            entries={models}
            currentPath={model?.path ?? null}
            library="Captures"
            emptyLabel="No captures in the library"
            onSelect={(path) => void loadModel(path)}
            onClear={() => void clearModel()}
            onEmptyStep={() => toast("No captures in the library yet. Import one.", "warning")}
            labels={{
              menu: "Choose a capture from the library",
              prev: "Previous capture",
              next: "Next capture",
              clear: "Remove capture",
            }}
            onBrowse={() => openT3kBrowser("models", "amp")}
            secondary={
              <Button variant="ghost" onClick={() => void importModel()}>
                Import file
              </Button>
            }
            downloads={active}
          />
        }
        display={showDisplay ? <ToneStackPlot off={toneOff} /> : undefined}
      >
        {layout === "stack" ? (
          <>
            <Row>
              <Group label="Level">
                <K id="amp_input" label="Input" />
                <K id="amp_output" label="Output" />
                {calLevel}
                {slim}
              </Group>
            </Row>
            <Row>{tone}</Row>
          </>
        ) : (
          <>
            <Row>
              <Group label="Level">
                <K id="amp_input" label="Input" size={size} />
                <K id="amp_output" label="Output" size={size} />
              </Group>
              {tone}
            </Row>
            <Row>
              {calLevel}
              {slim}
            </Row>
          </>
        )}
      </Editor>
    </>
  );
}

/* ───────────────────────────────── display ─────────────────────────────── */

/* The engine's law (src/dsp/Eq.cpp, Classic type, which the amp's tone stack
   runs): a low shelf at 150 Hz, Q 0.707, 4 dB per knob unit; a bell at 425 Hz,
   3 dB per unit, Q 1.5 when cutting and 0.7 when boosting; a high shelf at
   1.8 kHz, Q 0.707, 2 dB per unit. 5 is flat. At its own frequency a shelf
   gives exactly half its gain (in dB) and the bell all of it: `atCentre`. */
const BANDS = [
  { id: "amp_eq_bass", label: "Bass", hz: 150, dbPerUnit: 4, atCentre: 0.5 },
  { id: "amp_eq_mid", label: "Mid", hz: 425, dbPerUnit: 3, atCentre: 1 },
  { id: "amp_eq_treble", label: "Treble", hz: 1800, dbPerUnit: 2, atCentre: 0.5 },
] as const;
const TOP = 21;
const BOTTOM = -21;

function toneDb(f: number, gains: readonly number[]): number {
  const [bass = 0, mid = 0, treble = 0] = gains;
  return (
    lowShelfDb(f, 150, bass) + bellDb(f, 425, mid, mid < 0 ? 1.5 : 0.7) + highShelfDb(f, 1800, treble)
  );
}

function ToneStackPlot({ off }: { off: boolean }) {
  const bass = useSliderParam("amp_eq_bass");
  const mid = useSliderParam("amp_eq_mid");
  const treble = useSliderParam("amp_eq_treble");
  const sliders = [bass, mid, treble];
  /* What is drawn travels to a value that changed without a hand on it (a
     preset, automation); see useEase. The drag still writes from the pointer. */
  const shown = [useEase(bass.scaled), useEase(mid.scaled), useEase(treble.scaled)];
  const gains = BANDS.map((b, i) => b.dbPerUnit * (shown[i]! - 5));
  const rawGains = BANDS.map((b, i) => b.dbPerUnit * (sliders[i]!.scaled - 5));

  return (
    <Plot aria-label="Amp tone stack">
      {({ w, h }) => {
        const y = (db: number): number => dbY(clamp(db, BOTTOM - 6, TOP + 6), h, TOP, BOTTOM);
        const points = curve(w, (x) => y(toneDb(xFreq(x, w, F_MIN, F_MAX), gains)));
        return (
          <>
            <FreqGrid w={w} h={h} />
            <HRules w={w} rules={[{ y: y(0), axis: true }]} />
            <path className={p.fill} d={areaPath(points, y(0))} />
            <path className={cx(p.live, off && p.liveOff)} d={linePath(points)} />
            {BANDS.map((b, i) => {
              const slider = sliders[i]!;
              /* The handle sits on the curve; dragging it puts the curve under
                 the pointer, the other two bands' share held. */
              const total = toneDb(b.hz, gains);
              const others = toneDb(b.hz, rawGains) - b.atCentre * rawGains[i]!;
              return (
                <PlotHandle
                  key={b.id}
                  x={freqX(b.hz, w)}
                  y={y(total)}
                  label={b.label}
                  sliders={[slider]}
                  disabled={off}
                  onMove={(_x, py) => {
                    const gain = (lerp(py, 0, h, TOP, BOTTOM) - others) / b.atCentre;
                    slider.setScaled(clamp(5 + gain / b.dbPerUnit, 0, 10));
                  }}
                />
              );
            })}
          </>
        );
      }}
    </Plot>
  );
}
