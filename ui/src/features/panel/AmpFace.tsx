/**
 * The face: the `lead` of the amp and cab editors — the capture or IR that is
 * loaded, shown as the gear it was taken from. `kind` picks the wording and
 * the default drawing; everything else is the same component.
 *
 *   ┌──────────────────────────┐   the tone's photo (TONE3000 keeps one per
 *   │        the amp           │   tone; the plugin saves it beside the file),
 *   └──────────────────────────┘   or the default amp when there is none
 *   JCM800 2203 — Full Stack     ‹ › ⋯
 *   by tonehound · TONE3000
 *
 * Nothing loaded: the default amp, unlit, saying "Choose an amp" (for the cab:
 * the default cabinet, "Choose an IR"). The picture is the button: it (and ⋯)
 * opens one menu with where files come from (Browse TONE3000, Import file),
 * the library, and Remove.
 * ‹ › step through the library without opening anything.
 */
import { useState } from "react";
import { AnimatePresence, motion, useReducedMotion } from "motion/react";
import {
  AlertIcon,
  ChevronLeftIcon,
  ChevronRightIcon,
  IconButton,
  Menu,
  MoreIcon,
  Tooltip,
  cx,
  type MenuEntry,
} from "../../components";
import type { FileEntry } from "../../bridge";
import { duration, ease } from "../../theme/motion";
import { indexOfPath, stepIndex } from "./library";
import { leafOf, type MetaFact } from "./MediaMeta";
import f from "./ampFace.module.css";

/** What each kind of face calls its files. */
const WORDS = {
  amp: {
    menu: "Choose a capture from the library",
    section: "Captures",
    empty: "No captures in the library",
    remove: "Remove capture",
    prev: "Previous capture",
    next: "Next capture",
    call: "Choose an amp",
  },
  cab: {
    menu: "Choose an IR from the library",
    section: "IRs",
    empty: "No IRs in the library",
    remove: "Remove IR",
    prev: "Previous IR",
    next: "Next IR",
    call: "Choose an IR",
  },
} as const;

export interface AmpFaceProps {
  /** Whose face this is: the amp's (the default) or the cab's. */
  kind?: keyof typeof WORDS;
  /** The loaded capture's title, or null when the slot is empty. */
  title: string | null;
  /** Full title / path for the tooltip (the title truncates). */
  titleTip?: string;
  /** The capture's photo; null shows the default amp. */
  photo: string | null;
  /** Path the session asked for that is not on this machine (slot empty). */
  missing: string | null;
  /** The line under the title. */
  facts: readonly (MetaFact | null | false | undefined)[];
  entries: readonly FileEntry[];
  currentPath: string | null;
  onSelect(path: string): void;
  onClear(): void;
  /** Stepping an empty library is a no-op natively; we surface it instead. */
  onEmptyStep(): void;
  /** Where captures come from, first in the menu ("Browse TONE3000", …). */
  sources: readonly { id: string; label: string; onSelect(): void }[];
  /** TONE3000 downloads in flight for this slot. */
  downloads?: readonly { id: string; name: string; progress: number }[];
  /** Called with the root element (for `useBandWidth`). */
  onRoot?: (el: HTMLDivElement | null) => void;
}

const MISSING_TIP = "File not found on this computer";

export function AmpFace({ onRoot, kind = "amp", ...props }: AmpFaceProps) {
  const words = WORDS[kind];
  const { title, missing, entries, currentPath } = props;
  const reduced = useReducedMotion() ?? false;
  const [anchor, setAnchor] = useState<HTMLElement | null>(null);
  const [open, setOpen] = useState(false);
  /* A photo the WebView could not load falls back to the default amp. */
  const [failed, setFailed] = useState<string | null>(null);

  const loaded = title !== null;
  const lost = !loaded && missing !== null;
  const photo = loaded && props.photo && props.photo !== failed ? props.photo : null;
  const facts = props.facts.filter((fact): fact is MetaFact => !!fact);
  const download = props.downloads?.[0];

  const heading = loaded ? title : lost ? leafOf(missing) : null;
  const tip = loaded ? (props.titleTip ?? title) : lost ? `${missing} — ${MISSING_TIP}` : null;
  const line: MetaFact[] = download
    ? [{ key: "download", node: `Downloading ${download.name}` }]
    : lost
      ? [{ key: "missing", node: MISSING_TIP, warn: true }]
      : facts;

  const openFrom = (el: HTMLElement): void => {
    setAnchor(el);
    setOpen((was) => !was);
  };

  const step = (delta: number): void => {
    const next = stepIndex(indexOfPath(entries, currentPath), delta, entries.length);
    if (next === null) props.onEmptyStep();
    else props.onSelect(entries[next]!.path);
  };

  const menu: MenuEntry[] = [
    ...props.sources.map((source) => ({ ...source })),
    { kind: "separator" as const, id: "sep-library" },
    { kind: "section" as const, id: "library", label: words.section },
    ...(entries.length === 0
      ? [{ id: "empty", label: words.empty, disabled: true }]
      : entries.map((entry) => ({
          id: entry.path,
          label: entry.name,
          selected: entry.path === currentPath,
          onSelect: () => props.onSelect(entry.path),
        }))),
    ...(currentPath === null
      ? []
      : [
          { kind: "separator" as const, id: "sep-remove" },
          { id: "remove", label: words.remove, destructive: true, onSelect: props.onClear },
        ]),
  ];

  const fade = {
    initial: { opacity: 0 },
    animate: { opacity: 1 },
    exit: { opacity: 0 },
    transition: { duration: reduced ? 0 : duration.base, ease: ease.standard },
  };

  return (
    <div className={f.face} ref={onRoot}>
      <button
        type="button"
        className={f.stage}
        aria-label={words.menu}
        aria-haspopup="menu"
        aria-expanded={open}
        data-state={loaded ? "loaded" : "empty"}
        onClick={(e) => openFrom(e.currentTarget)}
      >
        {/* A capture that finishes loading crossfades over the old one rather
            than cutting; nothing animates on mount. */}
        <AnimatePresence initial={false}>
          <motion.span key={photo ?? (loaded ? "plain" : "empty")} className={f.picture} {...fade}>
            {photo ? (
              <img
                className={f.photo}
                src={photo}
                alt=""
                draggable={false}
                onError={() => setFailed(photo)}
              />
            ) : kind === "cab" ? (
              <DefaultCab lit={loaded} />
            ) : (
              <DefaultAmp lit={loaded} />
            )}
          </motion.span>
        </AnimatePresence>
        {!loaded && <span className={f.call}>{words.call}</span>}
        {download && (
          <span className={f.progress}>
            <span className={f.progressFill} style={{ transform: `scaleX(${download.progress})` }} />
          </span>
        )}
      </button>

      <div className={f.plate}>
        <div className={f.identityStack}>
          <AnimatePresence initial={false}>
            <motion.div
              key={`${loaded ? "file" : lost ? "lost" : "empty"}:${heading ?? ""}`}
              className={f.identity}
              {...fade}
              exit={{ opacity: 0, pointerEvents: "none" }}
            >
              {heading !== null &&
                (() => {
                  const node = (
                    <span className={f.title}>
                      {lost && <AlertIcon size={14} className={f.warnIcon} />}
                      <span className={f.titleText}>{heading}</span>
                    </span>
                  );
                  return tip ? (
                    <Tooltip label={tip} className={f.titleAnchor}>
                      {node}
                    </Tooltip>
                  ) : (
                    node
                  );
                })()}
              <div className={f.meta} role={lost ? "status" : undefined}>
                {line.map((fact, i) => (
                  <span key={fact.key} className={f.metaItem}>
                    {i > 0 && <span className={f.metaSep}>·</span>}
                    <span className={cx(f.metaItem, fact.warn && f.metaWarn)}>{fact.node}</span>
                  </span>
                ))}
              </div>
            </motion.div>
          </AnimatePresence>
        </div>

        <div className={f.steps}>
          <Tooltip label={words.prev}>
            <IconButton size="sm" aria-label={words.prev} onClick={() => step(-1)}>
              <ChevronLeftIcon size={14} />
            </IconButton>
          </Tooltip>
          <Tooltip label={words.next}>
            <IconButton size="sm" aria-label={words.next} onClick={() => step(1)}>
              <ChevronRightIcon size={14} />
            </IconButton>
          </Tooltip>
          <Tooltip label={words.section}>
            <IconButton
              size="sm"
              aria-label={words.section}
              aria-haspopup="menu"
              aria-expanded={open}
              onClick={(e) => openFrom(e.currentTarget)}
            >
              <MoreIcon size={14} />
            </IconButton>
          </Tooltip>
        </div>
      </div>

      <Menu
        anchor={anchor}
        open={open}
        onClose={() => setOpen(false)}
        entries={menu}
        minWidth={240}
      />
    </div>
  );
}

/**
 * The amp shown when a capture has no photo of its own (a file imported by
 * hand, a tone without one) and when nothing is loaded: a plain amp head. Its
 * pilot lamp is the block's accent while a capture is loaded and dark when the
 * slot is empty — the one mark on it that means something.
 */
function DefaultAmp({ lit }: { lit: boolean }) {
  const knobs = [0, 1, 2, 3, 4, 5];
  return (
    <svg className={f.amp} viewBox="22 8 276 156" preserveAspectRatio="xMidYMid meet" aria-hidden="true">
      <rect className={f.ampHandle} x="138" y="17" width="44" height="6" rx="3" />
      <rect className={f.ampBody} x="40.5" y="22.5" width="239" height="127" rx="8" />
      <rect className={f.ampCloth} x="52.5" y="34.5" width="215" height="70" rx="3" />
      <rect className={f.ampPanel} x="52.5" y="113.5" width="215" height="26" rx="3" />
      <circle className={f.ampJack} cx="68" cy="126.5" r="3.5" />
      {knobs.map((i) => {
        const x = 96 + i * 26;
        return (
          <g key={i}>
            <circle className={f.ampKnob} cx={x} cy="126.5" r="6" />
            <line className={f.ampPointer} x1={x} y1="126.5" x2={x} y2="121.5" />
          </g>
        );
      })}
      <circle className={cx(f.ampLamp, lit && f.ampLampLit)} cx="252" cy="126.5" r="3" />
      <rect className={f.ampFoot} x="58" y="150" width="18" height="4" rx="1" />
      <rect className={f.ampFoot} x="244" y="150" width="18" height="4" rx="1" />
    </svg>
  );
}

/**
 * The cab's counterpart: a plain 2x12 cabinet for an IR without a photo and
 * for an empty cab. Drawn in the amp's box so the call sits on its grille the
 * same way; the badge is its lamp.
 */
function DefaultCab({ lit }: { lit: boolean }) {
  return (
    <svg className={f.amp} viewBox="22 8 276 156" preserveAspectRatio="xMidYMid meet" aria-hidden="true">
      <rect className={f.ampHandle} x="138" y="17" width="44" height="6" rx="3" />
      <rect className={f.ampBody} x="40.5" y="22.5" width="239" height="127" rx="8" />
      <rect className={f.ampCloth} x="52.5" y="34.5" width="215" height="103" rx="3" />
      {/* The speakers show once an IR is loaded; empty, the grille is left
          clear for the call. */}
      {lit &&
        [112, 208].map((x) => (
          <g key={x}>
            <circle className={f.ampKnob} cx={x} cy="86" r="38" />
            <circle className={f.ampJack} cx={x} cy="86" r="13" />
          </g>
        ))}
      <rect className={cx(f.ampLamp, lit && f.ampLampLit)} x="150" y="128" width="20" height="4" rx="2" />
      <rect className={f.ampFoot} x="58" y="150" width="18" height="4" rx="1" />
      <rect className={f.ampFoot} x="244" y="150" width="18" height="4" rx="1" />
    </svg>
  );
}
