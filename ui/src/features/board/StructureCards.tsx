/**
 * The Split and Mixer tiles: ordinary block tiles whose second line says what
 * the region is doing, in plain text, and whose menu offers the whole-region
 * actions.
 *
 * Their own components on purpose: the lines read live parameters (the split
 * mode and crossover, five mixer values), and subscribing here keeps a knob
 * turned in the editor from re-rendering the whole board — only the one tile
 * whose line changed renders.
 */
import { useMemo } from "react";
import type { MenuEntry } from "../../components";
import { useSliderParam, useToggleParam } from "../../hooks";
import { useStore } from "../../store";
import { BlockCard, type BlockCardProps } from "./BlockCard";
import {
  SPLIT_LR,
  SPLIT_XOVER,
  formatHz,
  formatLevelShort,
  formatPan,
  useLaneNames,
  useXoverHz,
} from "./lanes";
import { splitRemoveEntries, swapSplitLanes } from "./splitActions";

type StructureCardProps = Omit<
  BlockCardProps,
  "detail" | "removeEntries" | "extraEntries"
>;

function useRemoveEntries(names: ReturnType<typeof useLaneNames>["names"]) {
  const order = useStore((st) => st.chainOrder);
  return useMemo(() => splitRemoveEntries(order, names), [names, order]);
}

/** "Copy", "L/R", "Crossover 800 Hz"; "Lane B muted" while it is off. */
export function SplitCard(props: StructureCardProps) {
  const { mode, names } = useLaneNames();
  const xover = useXoverHz();
  const on = useToggleParam("split_on").value;
  const removeEntries = useRemoveEntries(names);

  const detail = !on
    ? `Lane ${names.b} muted`
    : mode === SPLIT_LR
      ? "L/R"
      : mode === SPLIT_XOVER
        ? `Crossover ${formatHz(xover)}`
        : "Copy";

  return <BlockCard {...props} detail={detail} removeEntries={removeEntries} />;
}

/** One lane's slice of the mixer line: "A C", "B Ø 100R", "A -6 dB 40L". */
function laneReadout(name: string, db: number, pan: number, invert: boolean): string {
  const parts = [name];
  if (invert) parts.push("Ø");
  const level = formatLevelShort(db);
  if (level) parts.push(level);
  parts.push(formatPan(pan));
  return parts.join(" ");
}

/** "A 100L · B 100R"; "Mono sum" in a mono-out instance; "Unity sum" while
 *  the mixer is off. */
export function MixCard(props: StructureCardProps) {
  const { names } = useLaneNames();
  const aLevel = useSliderParam("mix_alevel").scaled;
  const bLevel = useSliderParam("mix_blevel").scaled;
  const aPan = useSliderParam("mix_apan").scaled;
  const bPan = useSliderParam("mix_bpan").scaled;
  const invert = useToggleParam("mix_bphase").value;
  const on = useToggleParam("mix_on").value;
  const removeEntries = useRemoveEntries(names);

  // A mono-out instance sums the lanes into one channel, so pans do nothing.
  const monoOut = useStore((st) => st.width?.host.out === 1);

  const detail = !on
    ? "Unity sum"
    : monoOut
      ? "Mono sum"
      : `${laneReadout(names.a, aLevel, aPan, false)} · ${laneReadout(names.b, bLevel, bPan, invert)}`;

  const extraEntries = useMemo<MenuEntry[]>(
    () => [
      {
        id: "swap-lanes",
        label: "Swap lanes",
        icon: <SwapGlyph />,
        onSelect: swapSplitLanes,
      },
    ],
    [],
  );

  return (
    <BlockCard
      {...props}
      detail={detail}
      extraEntries={extraEntries}
      removeEntries={removeEntries}
    />
  );
}

/** Two opposed arrows, drawn on the 14px menu-icon grid. */
export function SwapGlyph({ size = 14 }: { size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 16 16" aria-hidden="true">
      <path
        d="M5 13V3M5 3 2.5 5.5M5 3l2.5 2.5M11 3v10m0 0-2.5-2.5M11 13l2.5-2.5"
        fill="none"
        stroke="currentColor"
        strokeWidth={1.5}
        strokeLinecap="round"
        strokeLinejoin="round"
      />
    </svg>
  );
}
