/**
 * The amp editor, one for both instances of the amp kind (`amp`, `amp2`):
 * same layout, same controls, each bound to that instance's own parameters
 * (ui/DESIGN.md §Editors).
 *
 *   header   Output mode, Calibrate input
 *   lead     the amp's face: the capture's photo (or the default amp), its
 *            title and creator, the library's steps and menu (./AmpFace.tsx)
 *   display  the tone stack's response, a handle on each band (when it fits)
 *   rows     Level (Input, Output) and Tone (Bass, Mid, Treble, its switch),
 *            then Cal level and Slim
 *
 * TONE3000 download state lives in the store, so a download survives switching
 * to another block; errors surface as toasts from the store's actions.
 */
import { cx } from "../../components";
import type { AmpToken } from "../../bridge";
import { useSliderParam, useToggleParam } from "../../hooks";
import { AMP_SLOTS, ampCombo, ampSlider, ampToggle, useStore } from "../../store";
import { AmpFace } from "./AmpFace";
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
import { BareSwitch, DISPLAY_ROOM, controlsWidth, useBandWidth } from "./media";
import { ChoiceMenu, Editor, Group, HeaderSlot, K, Row, SwitchField } from "./ui";

/* Row widths at the toolkit's fixed cells (68 small, 84 large, 16px gaps). */
const ROW1_LG = 5 * 84 + 16;
const ROW1_SM = 5 * 68 + 16;

export function AmpBody({ block }: { block: AmpToken }) {
  const slot = AMP_SLOTS[block];
  const model = useStore(slot.model);
  const missing = useStore((st) => st.missingMedia[slot.missing]);
  const models = useStore((st) => st.models);
  const downloads = useStore((st) => st.downloads);
  const pending = useStore((st) => st.t3kPending);
  const load = useStore((st) => st.loadModel);
  const clear = useStore((st) => st.clearModel);
  const importModel = useStore((st) => st.importModel);
  const openT3kBrowser = useStore((st) => st.openT3kBrowser);
  const toast = useStore((st) => st.toast);

  const calInput = useToggleParam(ampToggle(block, "cal_input"));
  const eqOn = useToggleParam(ampToggle(block, "eq_on"));
  const attribution = useAttribution(model?.tone);
  const [band, bandRef] = useBandWidth();

  /* IR downloads share the progress map and belong to the cab; a capture's
     download shows on the amp it is headed for. */
  const active = Object.entries(downloads)
    .filter(([id]) => {
      const item = pending[Number(id)];
      return item?.kind !== "wav" && (item?.target ?? "amp") === block;
    })
    .map(([id, progress]) => ({ id, progress, name: pending[Number(id)]?.name ?? `Capture ${id}` }));

  const room = controlsWidth(band);
  const showDisplay = room >= ROW1_LG + DISPLAY_ROOM;
  const layout = room >= ROW1_LG ? "lg" : room >= ROW1_SM ? "sm" : "stack";
  const size = layout === "lg" ? "lg" : "sm";
  const toneOff = !eqOn.value;

  const tone = (
    <Group label="Tone" action={<BareSwitch id={ampToggle(block, "eq_on")} label="Enable amp EQ" />}>
      <K id={ampSlider(block, "eq_bass")} label="Bass" size={size} disabled={toneOff} />
      <K id={ampSlider(block, "eq_mid")} label="Mid" size={size} disabled={toneOff} />
      <K id={ampSlider(block, "eq_treble")} label="Treble" size={size} disabled={toneOff} />
    </Group>
  );
  const calLevel = <K id={ampSlider(block, "cal_level")} label="Cal level" disabled={!calInput.value} />;
  const slim = <K id={ampSlider(block, "slim")} label="Slim" disabled={!model?.isSlimmable} />;
  const input = ampSlider(block, "input");
  const output = ampSlider(block, "output");

  return (
    <>
      <HeaderSlot>
        <ChoiceMenu id={ampCombo(block, "out_mode")} label="Output" width={120} />
        <SwitchField id={ampToggle(block, "cal_input")} label="Calibrate input" />
      </HeaderSlot>
      <Editor
        lead={
          <AmpFace
            onRoot={bandRef}
            title={model ? titleOf(model) : null}
            titleTip={model ? (model.tone?.title ? `${model.tone.title} — ${model.name}` : model.path) : undefined}
            photo={model?.tone?.imageUrl ?? null}
            missing={missing}
            facts={[attribution, rigFact(model?.includesCab)]}
            entries={models}
            currentPath={model?.path ?? null}
            onSelect={(path) => void load(path, block)}
            onClear={() => void clear(block)}
            onEmptyStep={() => toast("No captures in the library yet. Import one.", "warning")}
            sources={[
              {
                id: "browse",
                label: "Browse TONE3000",
                onSelect: () => openT3kBrowser("models", block),
              },
              { id: "import", label: "Import file", onSelect: () => void importModel(block) },
            ]}
            downloads={active}
          />
        }
        display={showDisplay ? <ToneStackPlot block={block} off={toneOff} /> : undefined}
      >
        {layout === "stack" ? (
          <>
            <Row>
              <Group label="Level">
                <K id={input} label="Input" />
                <K id={output} label="Output" />
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
                <K id={input} label="Input" size={size} />
                <K id={output} label="Output" size={size} />
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
  { key: "eq_bass", label: "Bass", hz: 150, dbPerUnit: 4, atCentre: 0.5 },
  { key: "eq_mid", label: "Mid", hz: 425, dbPerUnit: 3, atCentre: 1 },
  { key: "eq_treble", label: "Treble", hz: 1800, dbPerUnit: 2, atCentre: 0.5 },
] as const;
const TOP = 21;
const BOTTOM = -21;

function toneDb(f: number, gains: readonly number[]): number {
  const [bass = 0, mid = 0, treble = 0] = gains;
  return (
    lowShelfDb(f, 150, bass) + bellDb(f, 425, mid, mid < 0 ? 1.5 : 0.7) + highShelfDb(f, 1800, treble)
  );
}

function ToneStackPlot({ block, off }: { block: AmpToken; off: boolean }) {
  const bass = useSliderParam(ampSlider(block, "eq_bass"));
  const mid = useSliderParam(ampSlider(block, "eq_mid"));
  const treble = useSliderParam(ampSlider(block, "eq_treble"));
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
                  key={b.key}
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
