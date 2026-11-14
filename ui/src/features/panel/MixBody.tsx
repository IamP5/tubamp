/**
 * Mixer editor (docs/SPLIT.md §2/§3): the two lanes, then the master. No
 * display — there is nothing to draw that the knobs do not already say.
 *
 *   Lane A            Lane B   Invert phase  │  Master
 *   Level  Pan        Level  Pan             │  Level
 *
 * Lanes are named per split mode (A/B, L/R, Low/High — features/board/lanes.ts)
 * and never coloured. The phase flip only ever applies to lane B, so it is that
 * group's switch. Pans read C / 33L / 33R through the Knob's `paramValue`.
 * Not instance-aware: mix is a singleton.
 */
import { useLaneNames } from "../board/lanes";
import { Divider, Editor, Group, K, Row, SwitchField } from "./ui";
import l from "./lanes.module.css";

export function MixBody() {
  const { names } = useLaneNames();
  return (
    <Editor>
      <Row className={l.edge}>
        <Group label={`Lane ${names.a}`}>
          <K id="mix_alevel" label="Level" size="lg" />
          <K id="mix_apan" label="Pan" />
        </Group>
        <Group label={`Lane ${names.b}`} action={<SwitchField id="mix_bphase" label="Invert phase" />}>
          <K id="mix_blevel" label="Level" size="lg" />
          <K id="mix_bpan" label="Pan" />
        </Group>
        <Divider />
        <Group label="Master">
          <K id="mix_level" label="Level" size="lg" />
        </Group>
      </Row>
    </Editor>
  );
}
