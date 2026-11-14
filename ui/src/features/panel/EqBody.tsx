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
 */
import type { BlockId, ComboParamId, SliderParamId } from "../../bridge";
import { cx } from "../../components";
import { useComboParam } from "../../hooks";
import { EqPlot, GRAPHIC, GRAPHIC_BANDS, PARAMETRIC, STACK } from "./eqDisplay";
import { ChoiceMenu, Editor, Group, HeaderSlot, K, Row, TypeStepper } from "./ui";
import s from "./eq.module.css";

export function EqBody({ block = "eq" }: { block?: BlockId }) {
  const id = (key: string): SliderParamId => `${block}_${key}` as SliderParamId;
  const type = useComboParam(`${block}_type` as ComboParamId);

  return (
    <>
      <HeaderSlot>
        <TypeStepper id={`${block}_type` as ComboParamId} label="Type" />
        {type.index === STACK && <ChoiceMenu id={`${block}_voice` as ComboParamId} label="Voice" width={130} />}
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
