/**
 * EQ editor (docs/FX-ENGINES.md §4.4). Instance-aware: each EQ block edits its
 * own `<token>_*` ids. Every type is neutral at the frozen defaults, so picking
 * one moves no knobs (no starting points).
 *
 * The curve is the editor: one response plot with a handle per band. Classic
 * and Stack: handles on bass, mid and treble. Parametric: the mid bell (frequency
 * and gain), the two shelves and the two cuts; Mid Q, Tilt and Level are knobs
 * only. Graphic: the seven bands are handles on the plot and nowhere else —
 * except at narrow widths, where the toolkit drops the display and the bands
 * become a row of small knobs.
 *
 * The header's Analyzer switch shows or hides the spectrum behind the curve.
 * While it is on, Freeze holds the spectrum at its maximum, and a "More" menu
 * beside it keeps the two settings that are set once and left: Resolution (the
 * engine's FFT size) and Speed (how fast the spectrum falls). The preferences
 * live in the store and are saved with the plugin's state; Freeze is not.
 */
import type { BlockId, ComboParamId, EqAnalyserStep, SliderParamId } from "../../bridge";
import { cx, type MenuEntry } from "../../components";
import { useContext } from "react";
import { useComboParam } from "../../hooks";
import { useStore } from "../../store";
import { EqPlot, GRAPHIC, GRAPHIC_BANDS, PARAMETRIC, STACK } from "./eqDisplay";
import {
  ChoiceMenu,
  ControlPlate,
  Editor,
  Group,
  HeaderSlot,
  K,
  LatchButton,
  MoreMenu,
  NarrowContext,
  Row,
  TypeStepper,
  ViewSwitch,
} from "./ui";
import s from "./eq.module.css";

/** Names of the analyser's three steps, per setting. */
const STEP_NAMES = {
  resolution: ["Low", "Medium", "High"],
  speed: ["Slow", "Medium", "Fast"],
} as const;

/** One setting as a labelled section of radio items, a check on the current one. */
function steps(
  key: keyof typeof STEP_NAMES,
  label: string,
  current: EqAnalyserStep,
  pick: (step: EqAnalyserStep) => void,
): MenuEntry[] {
  return [
    { kind: "section", id: `analyser:${key}`, label },
    ...STEP_NAMES[key].map((name, i) => ({
      id: `analyser:${key}:${i}`,
      label: name,
      radio: true,
      selected: i === current,
      onSelect: () => pick(i as EqAnalyserStep),
    })),
  ];
}

export function EqBody({ block = "eq" }: { block?: BlockId }) {
  const id = (key: string): SliderParamId => `${block}_${key}` as SliderParamId;
  const type = useComboParam(`${block}_type` as ComboParamId);
  const analyser = useStore((st) => st.eqAnalyser);
  const setAnalyser = useStore((st) => st.setEqAnalyser);
  const freeze = useStore((st) => st.eqFreeze);
  const setFreeze = useStore((st) => st.setEqFreeze);
  /* Narrow bands drop the display, and with it anything to analyse into. */
  const narrow = useContext(NarrowContext);

  return (
    <>
      <HeaderSlot>
        <TypeStepper id={`${block}_type` as ComboParamId} label="Type" />
        {type.index === STACK && <ChoiceMenu id={`${block}_voice` as ComboParamId} label="Voice" width={130} />}
        {!narrow && (
          <ViewSwitch
            label="Analyzer"
            checked={analyser.on}
            onChange={(on) => setAnalyser({ on })}
            title="The spectrum entering and leaving the EQ, behind the curve"
          />
        )}
        {!narrow && analyser.on && (
          <ControlPlate label="Analyzer">
            <LatchButton
              label="Freeze"
              pressed={freeze}
              onChange={setFreeze}
              title="Freeze the spectrum at its maximum"
            />
            <MoreMenu
              label="Analyzer settings"
              entries={[
                ...steps("resolution", "Resolution", analyser.resolution, (resolution) => setAnalyser({ resolution })),
                { kind: "separator", id: "analyser:sep" },
                ...steps("speed", "Speed", analyser.speed, (speed) => setAnalyser({ speed })),
              ]}
            />
          </ControlPlate>
        )}
      </HeaderSlot>
      <Editor display={<EqPlot block={block} type={type.index} />}>
        {type.index === GRAPHIC ? (
          <>
            <Row className={s.edge}>
              <K id={id("level")} label="Level" size="lg" />
            </Row>
            <Row className={cx(s.narrowOnly, s.edge)}>
              {GRAPHIC_BANDS.map(([key, f, tick]) => (
                <K key={key} id={id(key)} label={f < 1000 ? `${tick} Hz` : `${tick.replace("k", "")} kHz`} />
              ))}
            </Row>
          </>
        ) : (
          <>
            <Row className={s.edge}>
              <K id={id("bass")} label="Bass" size="lg" />
              <K id={id("mid")} label="Mid" size="lg" />
              <K id={id("treble")} label="Treble" size="lg" />
            </Row>
            {type.index === STACK && (
              <Row className={s.edge}>
                <K id={id("level")} label="Level" />
              </Row>
            )}
            {type.index === PARAMETRIC && (
              <Row className={s.edge}>
                <Group>
                  <K id={id("mid_freq")} label="Mid freq" />
                  <K id={id("mid_q")} label="Mid Q" />
                </Group>
                <Group>
                  <K id={id("lowcut")} label="Low cut" />
                  <K id={id("highcut")} label="High cut" />
                </Group>
                <Group>
                  <K id={id("tilt")} label="Tilt" />
                  <K id={id("level")} label="Level" />
                </Group>
              </Row>
            )}
          </>
        )}
      </Editor>
    </>
  );
}
