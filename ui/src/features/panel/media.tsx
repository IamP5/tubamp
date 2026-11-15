/**
 * The media card: the `lead` of the amp and cab editors — what is
 * loaded in the block, named by its title, with the library one click away.
 *
 *   JCM800 2203 — Full Stack Roar              title (the tone's, else the file's)
 *   by tonehound · TONE3000 · Includes a cab   only facts that change what to do
 *   ‹ [Captures ▾] ›  ✕                         the library: menu, step, empty
 *   [Browse TONE3000]  Import file              where new files come from
 *
 * Empty, the title says so in `text-2` and Browse becomes the view's primary
 * button. A session that names a file this machine lacks shows that file's name
 * with a warning icon; the slot itself is empty and the same actions apply.
 */
import { useLayoutEffect, useState, type ReactNode } from "react";
import { AnimatePresence, motion, useReducedMotion } from "motion/react";
import { AlertIcon, Button, Toggle, Tooltip, cx } from "../../components";
import type { FileEntry, ToggleParamId } from "../../bridge";
import { useToggleParam } from "../../hooks";
import { duration, ease } from "../../theme/motion";
import { FileRow } from "./FileRow";
import { leafOf, type MetaFact } from "./MediaMeta";
import m from "./media.module.css";

export interface MediaCardProps {
  /** The loaded file's title, or null when the slot is empty. */
  title: string | null;
  /** Full title / path for the tooltip (the title truncates). */
  titleTip?: string;
  /** "No capture", "No IR". */
  emptyTitle: string;
  /** Path the session asked for that is not on this machine (slot empty). */
  missing: string | null;
  /** The second line. */
  facts: readonly (MetaFact | null | false | undefined)[];
  /** Library row. */
  entries: readonly FileEntry[];
  currentPath: string | null;
  library: string;
  emptyLabel: string;
  onSelect(path: string): void;
  onClear(): void;
  onEmptyStep(): void;
  labels: { menu: string; prev: string; next: string; clear: string };
  onBrowse(): void;
  /** The ghost action beside Browse ("Import file", "Copy from Amp"). */
  secondary: ReactNode;
  /** TONE3000 downloads in flight for this slot. */
  downloads?: readonly { id: string; name: string; progress: number }[];
  /** Called with the card's root element (for `useBandWidth`). */
  onRoot?: (el: HTMLDivElement | null) => void;
}

const MISSING_TIP = "File not found on this computer";

export function MediaCard({ onRoot, ...props }: MediaCardProps) {
  const { title, missing } = props;
  const reduced = useReducedMotion() ?? false;
  const facts = props.facts.filter((f): f is MetaFact => !!f);
  const loaded = title !== null;
  const lost = !loaded && missing !== null;

  const heading = loaded ? title : lost ? leafOf(missing) : props.emptyTitle;
  const tip = loaded ? (props.titleTip ?? title) : lost ? `${missing} — ${MISSING_TIP}` : null;
  const line: MetaFact[] = lost
    ? [{ key: "missing", node: MISSING_TIP, warn: true }]
    : facts;

  const titleNode = (
    <span className={cx(m.title, !loaded && !lost && m.titleEmpty)}>
      {lost && <AlertIcon size={14} className={m.warnIcon} />}
      <span className={m.titleText}>{heading}</span>
    </span>
  );

  return (
    <div className={m.card} ref={onRoot}>
      {/* A capture or IR that finishes loading (or is removed) crossfades in
          over the old one rather than cutting; nothing animates on mount. */}
      <div className={m.identityStack}>
        <AnimatePresence initial={false}>
          <motion.div
            key={`${loaded ? "file" : lost ? "lost" : "empty"}:${heading}`}
            className={m.identity}
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0, pointerEvents: "none" }}
            transition={{ duration: reduced ? 0 : duration.base, ease: ease.standard }}
          >
            {tip ? (
              <Tooltip label={tip} className={m.titleAnchor}>
                {titleNode}
              </Tooltip>
            ) : (
              titleNode
            )}
            <div className={m.meta} role={lost ? "status" : undefined}>
              {line.map((fact, i) => (
                <span key={fact.key} className={m.metaItem}>
                  {i > 0 && <span className={m.metaSep}>·</span>}
                  <span className={cx(m.metaItem, fact.warn && m.metaWarn)}>{fact.node}</span>
                </span>
              ))}
            </div>
          </motion.div>
        </AnimatePresence>
      </div>

      <FileRow
        entries={props.entries}
        currentPath={props.currentPath}
        library={props.library}
        emptyLabel={props.emptyLabel}
        onSelect={props.onSelect}
        onClear={props.onClear}
        onEmptyStep={props.onEmptyStep}
        labels={props.labels}
      />

      <div className={m.actions}>
        <Button variant={loaded ? "secondary" : "primary"} onClick={props.onBrowse}>
          Browse TONE3000
        </Button>
        {props.secondary}
      </div>

      {props.downloads?.map((d) => (
        <div key={d.id} className={m.download}>
          <span className={m.downloadName} title={d.name}>
            Downloading {d.name}
          </span>
          <div className={m.progress}>
            <div className={m.progressFill} style={{ transform: `scaleX(${d.progress})` }} />
          </div>
        </div>
      ))}
    </div>
  );
}

/**
 * The editor band's width, read from the band element around `el`. The amp
 * and cab editors carry a lead column the toolkit's single `data-narrow` step
 * does not account for, so they choose their own layout (display or not, large
 * or small knobs) from the real width.
 */
export function useBandWidth(): [number, (el: HTMLElement | null) => void] {
  const [el, setEl] = useState<HTMLElement | null>(null);
  const [width, setWidth] = useState(0);
  useLayoutEffect(() => {
    const band = el?.closest<HTMLElement>("[data-block]");
    if (!band) return;
    const measure = (): void => setWidth(band.clientWidth);
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(band);
    return () => observer.disconnect();
  }, [el]);
  return [width, setEl];
}

/** Space the controls get beside the lead, at band width `band` (mirrors the
 *  toolkit: 16px body padding each side, a 320px lead — 260 under 780px — and
 *  a 24px gap). */
export function controlsWidth(band: number): number {
  if (band <= 0) return Infinity;
  return band - 32 - (band < 780 ? 260 : 320) - 24;
}

/** The smallest a display may be (the toolkit's `.display` min-width) plus
 *  the gap before the controls. */
export const DISPLAY_ROOM = 240 + 24;

/**
 * A boolean parameter as a bare switch, for a group's caption line where the
 * caption already names it (the amp's Tone group). Local to the media editors:
 * the toolkit's `SwitchField` always prints its own caption.
 */
export function BareSwitch({ id, label }: { id: ToggleParamId; label: string }) {
  const param = useToggleParam(id);
  return (
    <span className={m.switch} data-param-index={param.parameterIndex} title={param.name}>
      <Toggle checked={param.value} onChange={param.setValue} aria-label={label} />
    </span>
  );
}
