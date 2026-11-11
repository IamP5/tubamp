/**
 * Level 2 — one tone. The file list is a second request (`t3kListModels`,
 * A2-filtered for captures), shared with the row's one-click Load through the
 * store's session cache, so opening a tone the user just loaded costs nothing.
 *
 * The primary Load takes the same default the row does (NAM: `standard`, else
 * the first; IR: the first); the file list shows that default first, with size
 * and architecture badges and which files are already in the library.
 */
import { useEffect, useMemo } from "react";
import {
  Button,
  CheckIcon,
  ChevronLeftIcon,
  CloseIcon,
  DownloadIcon,
  EmptyState,
  IconButton,
  Skeleton,
  Spinner,
} from "../../components";
import type { T3kTone } from "../../bridge";
import {
  bestDefaultFile,
  findInstalledPath,
  loadedPathOf,
  loadedTargetsOf,
  selectT3kTarget,
  T3K_TARGET_LABELS,
  useStore,
} from "../../store";
import {
  Avatar,
  ExternalIcon,
  FormatBadge,
  HeartButton,
  HeartIcon,
  RetryIcon,
  ToneArt,
} from "./parts";
import { ModelRow } from "./ModelRow";
import { formatCount, GEAR_LABELS, LICENSE_LABELS, toneUrl } from "./labels";
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
  const drawerTarget = useStore(selectT3kTarget);
  const target = tone.format === "ir" ? "cab" : drawerTarget === "cab" ? "amp" : drawerTarget;
  const loadedIn = useStore((st) => loadedTargetsOf(st, tone.id).join(","));

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

  const chips = [...tone.makes, ...tone.tags];
  const license = tone.license ? (LICENSE_LABELS[tone.license] ?? tone.license) : null;
  const loadedTargets = loadedIn === "" ? [] : loadedIn.split(",");

  return (
    <>
      <header className={s.head}>
        <IconButton aria-label="Back to browse" onClick={onBack} size="sm">
          <ChevronLeftIcon size={16} />
        </IconButton>
        <span className={s.headTitle}>{tone.title}</span>
        <IconButton aria-label="Close browser" onClick={onClose} size="sm">
          <CloseIcon size={14} />
        </IconButton>
      </header>

      <div className={s.scroll}>
        <ToneArt tone={tone} className={s.banner}>
          <span className={s.bannerScrim} />
          <span className={s.bannerTags}>
            <span className={s.gearTag}>{GEAR_LABELS[tone.gear] ?? tone.gear}</span>
            <FormatBadge tone={tone} />
            {loadedTargets.map((t) => (
              <span key={t} className={s.bannerLoaded}>
                In {T3K_TARGET_LABELS[t as keyof typeof T3K_TARGET_LABELS]}
              </span>
            ))}
          </span>
        </ToneArt>

        <div className={s.creatorRow}>
          <Avatar tone={tone} size={22} />
          <span className={s.creatorCol}>
            <span className={s.creatorName}>{tone.creator.username}</span>
            <span className={s.creatorStats}>
              <DownloadIcon size={11} />
              {formatCount(tone.downloadsCount)}
              <span className={s.dot}>·</span>
              <HeartIcon size={11} filled />
              {formatCount(tone.favoritesCount)}
            </span>
          </span>
          <HeartButton tone={tone} onToggle={toggleFavorite} />
        </div>

        <PrimaryLoad tone={tone} target={target} />

        {tone.description && <p className={s.description}>{tone.description}</p>}

        {chips.length > 0 && (
          <div className={s.tagRow}>
            {chips.map((chip) => (
              <span key={chip} className={s.tag}>
                {chip}
              </span>
            ))}
          </div>
        )}

        <div className={s.metaLine}>
          {license && (
            <span className={s.licenseLabel} title="Creator-chosen license on TONE3000">
              {license}
            </span>
          )}
          <button
            type="button"
            className={s.externalLink}
            onClick={() => void openExternal(toneUrl(tone))}
          >
            Open on tone3000.com
            <ExternalIcon size={11} />
          </button>
        </div>

        <div className={s.sectionHead}>
          <span>Files</span>
          <span className={s.sectionCount}>{models?.length ?? tone.modelsCount}</span>
        </div>

        {models === null && files?.error ? (
          <EmptyState
            className={s.filesEmpty}
            message="No files to show"
            hint={files.error}
            action={
              <Button size="sm" onClick={() => void listFiles(tone, true)}>
                Try again
              </Button>
            }
          />
        ) : models === null ? (
          <ul className={s.fileList} aria-busy="true">
            {[0, 1].map((i) => (
              <li key={i} className={s.file}>
                <Skeleton width="60%" height={11} />
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
      </div>
    </>
  );
}

/** The detail level's one big action: load the default file into the target. */
function PrimaryLoad({ tone, target }: { tone: T3kTone; target: "amp" | "amp2" | "cab" }) {
  const load = useStore((st) => st.t3kLoads[tone.id]);
  const progress = useStore((st) =>
    load?.modelId != null ? st.downloads[load.modelId] : undefined,
  );
  const files = useStore((st) => st.t3kFiles[tone.id]?.models ?? null);
  const fallback = files ? bestDefaultFile(files) : null;
  const loaded = useStore((st) => {
    if (!fallback) return false;
    const path = findInstalledPath(st, fallback);
    return path !== null && path === loadedPathOf(st, target);
  });
  const installed = useStore((st) => (fallback ? findInstalledPath(st, fallback) !== null : false));
  const loadTone = useStore((st) => st.t3kLoadTone);
  const label = T3K_TARGET_LABELS[target];
  const unavailable = files !== null && fallback === null;

  let content;
  if (load && load.phase !== "failed") {
    const pct = load.phase === "downloading" ? Math.round((progress ?? 0) * 100) : null;
    content = (
      <Button variant="primary" block disabled icon={<Spinner size={13} />}>
        {pct !== null
          ? `Downloading… ${pct}%`
          : load.phase === "resolving"
            ? "Finding files…"
            : `Loading into ${label}…`}
      </Button>
    );
  } else if (load?.phase === "failed") {
    content = (
      <Button
        variant="danger"
        block
        icon={<RetryIcon size={13} />}
        title={load.error ?? undefined}
        onClick={() => void loadTone(tone)}
      >
        Retry — load into {label}
      </Button>
    );
  } else if (loaded) {
    content = (
      <Button block disabled icon={<CheckIcon size={13} />}>
        Loaded in {label}
      </Button>
    );
  } else {
    content = (
      <Button
        variant="primary"
        block
        disabled={unavailable}
        icon={installed ? undefined : <DownloadIcon size={13} />}
        onClick={() => void loadTone(tone)}
      >
        {unavailable ? "Nothing to load" : `Load into ${label}`}
      </Button>
    );
  }

  return (
    <div className={s.primaryLoad}>
      {content}
      <span className={s.primaryHint}>
        {load?.phase === "failed" && load.error
          ? load.error
          : fallback
            ? `${fallback.name}${installed ? " · in your library" : ""}`
            : tone.format === "ir"
              ? "Loads the first IR of this tone"
              : "Loads the standard-size capture"}
      </span>
    </div>
  );
}
