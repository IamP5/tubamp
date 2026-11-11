/**
 * What the amp and cab panels say about the file they hold, beyond its name:
 * whose capture it is, what the cab made of an IR, and — when a session asked
 * for a file this machine does not have — that it is missing, and which one.
 */
import { AlertIcon, Tooltip } from "../../components";
import type { IrInfo, ToneRef } from "../../bridge";
import { useStore } from "../../store";
import s from "./panel.module.css";

/* ─────────────────────────────── attribution ───────────────────────────── */

/**
 * The caption row's right-hand side. A file downloaded from TONE3000 credits
 * its creator (the API terms want the name to stay visible, and it is the
 * useful thing to know) and links to the tone's page; anything else shows the
 * plain "Powered by TONE3000" the catalog entry points need.
 */
export function Attribution({ tone }: { tone?: ToneRef | null }) {
  const openExternal = useStore((st) => st.t3kOpenExternal);

  if (!tone || !tone.creator) {
    return <span className={s.attribution}>Powered by TONE3000</span>;
  }

  const label = `by ${tone.creator} · TONE3000`;
  if (!tone.url) return <span className={s.attribution}>{label}</span>;

  return (
    <Tooltip
      label={tone.title ? `${tone.title} — open on tone3000.com` : "Open on tone3000.com"}
      placement="top"
    >
      <button
        type="button"
        className={s.attributionLink}
        onClick={() => void openExternal(tone.url)}
      >
        {label}
      </button>
    </Tooltip>
  );
}

/* ───────────────────────────── missing file ────────────────────────────── */

function leafOf(path: string): string {
  const cut = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return cut >= 0 ? path.slice(cut + 1) : path;
}

/**
 * The slot is empty because the file the session (or preset) names is not on
 * this machine and did not travel with it. Says which file, so the user knows
 * what to look for; the reference is kept, so putting the file back — or
 * loading the session where it lives — restores the sound.
 */
export function MissingNotice({ path, what }: { path: string; what: string }) {
  return (
    <Tooltip
      label={`${path} — this ${what} is not on this machine. Load it again, or pick another; the session keeps the reference until you do.`}
      placement="top"
      className={s.rigNoticeAnchor}
    >
      <div className={s.rigNotice} role="status">
        <AlertIcon size={13} className={s.rigNoticeIcon} />
        <span className={s.rigNoticeText}>Missing: {leafOf(path)}</span>
      </div>
    </Tooltip>
  );
}

/* ─────────────────────────────── IR status ─────────────────────────────── */

const IMAGE_TIPS: Record<IrInfo["image"], string> = {
  none: "",
  stereo: "True stereo: the left channel runs through the IR's left side, the right through its right.",
  "mono-file": "This IR has one channel; it is applied to left and right alike.",
  "mono-path":
    "This IR is stereo, but an amp follows the cab and folds the signal to mono, so only its left side is used. Put the cab after the amp for true stereo.",
};

function formatSeconds(seconds: number): string {
  return seconds >= 1 ? `${seconds.toFixed(1)} s` : `${Math.round(seconds * 1000)} ms`;
}

/**
 * How the cab is applying the loaded IR: stereo or mono (and why), cabinet or
 * room, and the kernel's length. Sits in the caption row — the cab panel has no
 * spare line.
 */
export function IrStatus({ ir }: { ir: IrInfo }) {
  const stereo = ir.image === "stereo";
  // Before the cab has run a block it has no verdict yet; say what the file is.
  const imageLabel =
    ir.image === "none" ? (ir.channels > 1 ? "STEREO" : "MONO") : stereo ? "STEREO" : "MONO";
  const imageTip =
    ir.image === "none"
      ? ir.channels > 1
        ? "A two-channel IR."
        : IMAGE_TIPS["mono-file"]
      : IMAGE_TIPS[ir.image];

  return (
    <span className={s.irStatus}>
      <Tooltip label={imageTip} placement="top">
        <span className={stereo ? s.chipLive : ir.image === "mono-path" ? s.chipRig : s.chipPlain}>
          {imageLabel}
        </span>
      </Tooltip>
      {ir.reverbLike && (
        <Tooltip
          label="Longer than a second, or tagged as a space: a room rather than a cabinet. It keeps its full level — blend it in with MIX."
          placement="top"
        >
          <span className={s.chipPlain}>ROOM</span>
        </Tooltip>
      )}
      <span className={s.meta}>
        {formatSeconds(ir.seconds)}
        {ir.truncated ? " (cut at 10 s)" : ""}
      </span>
    </span>
  );
}
