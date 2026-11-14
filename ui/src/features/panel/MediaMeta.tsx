/**
 * What the media card says about the file it holds beyond its title: whose
 * tone it is, and only the facts that change what the user does next.
 */
import type { ReactNode } from "react";
import { Tooltip } from "../../components";
import type { IrInfo, ToneRef } from "../../bridge";
import { useStore } from "../../store";
import m from "./media.module.css";

/** One entry on the card's second line. */
export interface MetaFact {
  key: string;
  node: ReactNode;
  warn?: boolean;
}

/** The title a loaded file goes by: the TONE3000 tone's title when it came
 *  from there, otherwise the library's display name for the file. */
export function titleOf(file: { name: string; tone?: ToneRef | null }): string {
  return file.tone?.title || file.name;
}

export function leafOf(path: string): string {
  const cut = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  const leaf = cut >= 0 ? path.slice(cut + 1) : path;
  const dot = leaf.lastIndexOf(".");
  return dot > 0 ? leaf.slice(0, dot) : leaf;
}

/**
 * "by <creator> · TONE3000" for a file downloaded from TONE3000 (the API terms
 * want the creator visible), linking to the tone's page. Nothing otherwise.
 */
export function useAttribution(tone: ToneRef | null | undefined): MetaFact | null {
  const openExternal = useStore((st) => st.t3kOpenExternal);
  if (!tone?.creator) return null;
  const text = `by ${tone.creator} · TONE3000`;
  if (!tone.url) return { key: "by", node: text };
  return {
    key: "by",
    node: (
      <Tooltip label="Open on tone3000.com">
        <button type="button" className={m.link} onClick={() => void openExternal(tone.url)}>
          {text}
        </button>
      </Tooltip>
    ),
  };
}

const RIG_TIP =
  "This capture was taken through a cabinet: a Cab block after it adds a second one.";

/** A capture that already went through a cabinet. Absent gear data says
 *  nothing (most captures trained outside TONE3000 carry none). */
export function rigFact(includesCab: boolean | undefined): MetaFact | null {
  if (!includesCab) return null;
  return {
    key: "rig",
    node: (
      <Tooltip label={RIG_TIP}>
        <span>Includes a cab</span>
      </Tooltip>
    ),
  };
}

/** What the cab made of the IR, where it changes what to do: a stereo file
 *  folded to mono by the amp behind it, a room (wants Mix below 100 %), a
 *  file cut at the 10 s limit. */
export function irFacts(ir: IrInfo): MetaFact[] {
  const facts: MetaFact[] = [];
  if (ir.image === "mono-path") {
    facts.push({
      key: "mono",
      node: (
        <Tooltip label="An amp follows the cab and folds the signal to mono, so only the IR's left side is used. Put the cab after the amp for true stereo.">
          <span>Used in mono</span>
        </Tooltip>
      ),
    });
  }
  if (ir.reverbLike) {
    facts.push({
      key: "room",
      node: (
        <Tooltip label="Longer than a second, or tagged as a space: a room rather than a cabinet. It keeps its full level; blend it in with Mix.">
          <span>Room</span>
        </Tooltip>
      ),
    });
  }
  if (ir.truncated) facts.push({ key: "cut", node: "Cut at 10 s" });
  return facts;
}
