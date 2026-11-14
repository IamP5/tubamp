/**
 * Level 2 — one tone. The file list is a second request (`t3kListModels`,
 * A2-filtered for captures), shared with the row's one-click Load through the
 * store's session cache, so opening a tone the user just loaded costs nothing.
 *
 * The default file (NAM: `standard`, else the first; IR: the first — the one
 * the row's Load takes) is listed first. The head's chevrons step the target
 * through the pack's files, one load per click.
 */
import { useLayoutEffect, useMemo, useRef, useState, useEffect } from "react";
import {
  Button,
  ChevronDownIcon,
  ChevronLeftIcon,
  CloseIcon,
  EmptyState,
  IconButton,
  Skeleton,
} from "../../components";
import type { T3kTone } from "../../bridge";
import {
  bestDefaultFile,
  findInstalledPath,
  loadedPathOf,
  selectT3kTarget,
  useStore,
} from "../../store";
import { ExternalIcon, HeartButton } from "./parts";
import { ModelRow } from "./ModelRow";
import { GEAR_LABELS, includesCab, LICENSE_LABELS, toneUrl } from "./labels";
import s from "./t3k.module.css";

export interface ToneDetailProps {
  tone: T3kTone;
  onBack(): void;
  onClose(): void;
}

export function ToneDetail({ tone, onBack, onClose }: ToneDetailProps) {
  const files = useStore((st) => st.t3kFiles[tone.id]);
  const listFiles = useStore((st) => st.t3kListFiles);
  const toggleFavorite = useStore((st) => st.t3kToggleFavorite);
  const openExternal = useStore((st) => st.t3kOpenExternal);
  const loadTone = useStore((st) => st.t3kLoadTone);
  const drawerTarget = useStore(selectT3kTarget);
  const target = tone.format === "ir" ? "cab" : drawerTarget === "cab" ? "amp" : drawerTarget;
  const busy = useStore((st) => {
    const load = st.t3kLoads[tone.id];
    return load !== undefined && load.phase !== "failed";
  });

  // Keyed on the tone's identity, not the object: the drawer hands down a fresh
  // object whenever a favorite override is layered on, and a failed listing
  // would otherwise be re-requested on every such render, without a click.
  const { id: toneId, format: toneFormat } = tone;
  useEffect(() => {
    void listFiles({ id: toneId, format: toneFormat });
  }, [toneId, toneFormat, listFiles]);

  const models = files?.models ?? null;
  const fallback = useMemo(() => (models ? bestDefaultFile(models) : null), [models]);
  const sorted = useMemo(() => {
    if (!models || !fallback) return models ?? [];
    return [fallback, ...models.filter((m) => m.id !== fallback.id)];
  }, [models, fallback]);

  /* Which of the pack's files the target holds now (-1: none of them). */
  const current = useStore((st) => {
    const path = loadedPathOf(st, target);
    if (path === null) return -1;
    return sorted.findIndex((m) => findInstalledPath(st, m) === path);
  });
  const step = (delta: number): void => {
    const next = sorted[current + delta];
    if (next) void loadTone(tone, next);
  };

  const license = tone.license ? (LICENSE_LABELS[tone.license] ?? tone.license) : null;
  const gear = GEAR_LABELS[tone.gear] ?? tone.gear;

  return (
    <>
      <header className={s.head}>
        <IconButton aria-label="Back to browse" onClick={onBack} size="sm">
          <ChevronLeftIcon />
        </IconButton>
        <span className={s.headTitle} title={tone.title}>
          {tone.title}
        </span>
        {sorted.length > 1 && (
          <span className={s.headGroup}>
            <IconButton
              aria-label="Previous file"
              size="sm"
              disabled={busy || current <= 0}
              onClick={() => step(-1)}
            >
              <ChevronDownIcon className={s.up} />
            </IconButton>
            <IconButton
              aria-label="Next file"
              size="sm"
              disabled={busy || current >= sorted.length - 1}
              onClick={() => step(1)}
            >
              <ChevronDownIcon />
            </IconButton>
          </span>
        )}
        <IconButton aria-label="Close browser" onClick={onClose} size="sm">
          <CloseIcon />
        </IconButton>
      </header>

      <div className={s.scroll}>
        <div className={s.intro}>
          <div className={s.byline}>
            <span className={s.bylineText}>
              by <span className={s.creator}>{tone.creator.username}</span>
              {" · "}
              {gear}
              {includesCab(tone.gear) && " · includes a cab"}
            </span>
            <HeartButton tone={tone} onToggle={toggleFavorite} />
          </div>
          {tone.description && <Description text={tone.description} />}
        </div>

        <div className={s.sectionLabel}>Files</div>

        {models === null && files?.error ? (
          <EmptyState
            className={s.filesEmpty}
            message={files.error}
            action={
              <Button size="sm" onClick={() => void listFiles(tone, true)}>
                Retry
              </Button>
            }
          />
        ) : models === null ? (
          <ul className={s.fileList} aria-busy="true">
            {[0, 1].map((i) => (
              <li key={i} className={s.file}>
                <span className={s.fileMain}>
                  <Skeleton width="56%" height={12} radius="var(--ds-r-1)" />
                </span>
              </li>
            ))}
          </ul>
        ) : models.length === 0 ? (
          <EmptyState
            className={s.filesEmpty}
            message="No files to load"
            hint={
              tone.format === "ir"
                ? "This tone has no downloadable IRs."
                : "This tone has no A2 captures, which is all this plugin loads."
            }
          />
        ) : (
          <ul className={s.fileList}>
            {sorted.map((model) => (
              <ModelRow
                key={model.id}
                tone={tone}
                model={model}
                target={target}
                isDefault={model.id === fallback?.id}
              />
            ))}
          </ul>
        )}

        <div className={s.licence}>
          <span className={s.licenceText}>
            {license ? `${license} · ` : ""}
            {tone.creator.username} on TONE3000
          </span>
          <button
            type="button"
            className={s.external}
            onClick={() => void openExternal(toneUrl(tone))}
          >
            Open on tone3000.com
            <ExternalIcon />
          </button>
        </div>
      </div>
    </>
  );
}

/** The description, clamped to three lines with "More" when it runs longer. */
function Description({ text }: { text: string }) {
  const ref = useRef<HTMLParagraphElement>(null);
  const [open, setOpen] = useState(false);
  const [long, setLong] = useState(false);
  useLayoutEffect(() => {
    const el = ref.current;
    if (el && !open) setLong(el.scrollHeight > el.clientHeight + 1);
  }, [text, open]);

  return (
    <div className={s.description}>
      <p ref={ref} className={open ? undefined : s.clamp}>
        {text}
      </p>
      {(long || open) && (
        <button type="button" className={s.more} onClick={() => setOpen(!open)}>
          {open ? "Less" : "More"}
        </button>
      )}
    </div>
  );
}
