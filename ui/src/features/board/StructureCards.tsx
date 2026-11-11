/**
 * The SPLIT and MIX cards: ordinary block cards whose note says what the region
 * is doing, and whose kebab offers the whole-region actions.
 *
 * Their own components on purpose: the notes read live parameters (the split
 * mode and crossover, five mixer values), and subscribing here keeps a knob
 * turned in the panel from re-rendering the whole board — only the one card
 * whose read-out changed renders.
 */
import { useMemo } from "react";
import type { MenuEntry } from "../../components";
import { useSliderParam, useToggleParam } from "../../hooks";
import { useStore } from "../../store";
import { BlockCard, type BlockCardProps, type CardNote } from "./BlockCard";
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
  "note" | "removeEntries" | "extraEntries"
>;

function useRemoveEntries(names: ReturnType<typeof useLaneNames>["names"]) {
  const order = useStore((st) => st.chainOrder);
  return useMemo(() => splitRemoveEntries(order, names), [names, order]);
}

export function SplitCard(props: StructureCardProps) {
  const { mode, names } = useLaneNames();
  const xover = useXoverHz();
  const on = useToggleParam("split_on").value;
  const removeEntries = useRemoveEntries(names);

  const note = useMemo<CardNote>(() => {
    if (!on)
      return {
        label: `${names.b} MUTED`,
        tip: `The splitter is bypassed: lane ${names.b} is muted at the mixer and only lane ${names.a} is heard.`,
        tone: "info",
      };
    if (mode === SPLIT_LR)
      return {
        label: "L | R",
        tip: "L/R: lane L gets the left input channel, lane R the right.",
        tone: "info",
      };
    if (mode === SPLIT_XOVER)
      return {
        label: `X-OVER ${formatHz(xover)}`,
        tip: `Crossover: lane LOW gets everything below ${formatHz(xover)}, lane HIGH everything above.`,
        tone: "info",
      };
    return {
      label: "COPY",
      tip: "Copy: both lanes get the full signal.",
      tone: "info",
    };
  }, [mode, names, on, xover]);

  return <BlockCard {...props} note={note} removeEntries={removeEntries} />;
}

/** One lane's slice of the mixer read-out: "A C", "B Ø 100R", "A -6dB 40L". */
function laneReadout(name: string, db: number, pan: number, invert: boolean): string {
  const parts = [name];
  if (invert) parts.push("Ø");
  const level = formatLevelShort(db);
  if (level) parts.push(level);
  parts.push(formatPan(pan));
  return parts.join(" ");
}

export function MixCard(props: StructureCardProps) {
  const { names } = useLaneNames();
  const aLevel = useSliderParam("mix_alevel").scaled;
  const bLevel = useSliderParam("mix_blevel").scaled;
  const aPan = useSliderParam("mix_apan").scaled;
  const bPan = useSliderParam("mix_bpan").scaled;
  const invert = useToggleParam("mix_bphase").value;
  const on = useToggleParam("mix_on").value;
  const removeEntries = useRemoveEntries(names);

  const note = useMemo<CardNote>(() => {
    if (!on)
      return {
        label: "UNITY SUM",
        tip: "The mixer is bypassed: both lanes are summed at unity — level, pan and phase are ignored.",
        tone: "info",
      };
    // The pill is 112px of 8px mono: LOW / HIGH shorten to LO / HI there (the
    // tooltip and every other surface keep the full names).
    const short = (name: string) =>
      name === "LOW" ? "LO" : name === "HIGH" ? "HI" : name;
    const a = laneReadout(short(names.a), aLevel, aPan, false);
    const b = laneReadout(short(names.b), bLevel, bPan, invert);
    return {
      label: `${a} · ${b}`,
      tip: `Lane ${names.a}: ${aLevel.toFixed(1)} dB, pan ${formatPan(aPan)} · Lane ${names.b}: ${bLevel.toFixed(1)} dB, pan ${formatPan(bPan)}${invert ? ", phase inverted" : ""}`,
      tone: "info",
    };
  }, [aLevel, aPan, bLevel, bPan, invert, names, on]);

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
      note={note}
      extraEntries={extraEntries}
      removeEntries={removeEntries}
    />
  );
}

/** ⇅ — two opposed arrows, drawn on the 14px menu-icon grid. */
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
