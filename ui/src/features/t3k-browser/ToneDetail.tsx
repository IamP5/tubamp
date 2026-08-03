/**
 * Level 2 — one tone. The file list is a second request (`t3kListModels`,
 * A2-filtered for captures), so it carries its own loading and error states; the
 * tone itself is whatever level 1 already had.
 */
import { useEffect, useState } from "react";
import {
  Button,
  ChevronLeftIcon,
  CloseIcon,
  DownloadIcon,
  EmptyState,
  IconButton,
  Skeleton,
} from "../../components";
import { bridge, type T3kModel, type T3kTone } from "../../bridge";
import { useStore } from "../../store";
import { Avatar, FormatBadge, HeartButton, HeartIcon, ToneArt } from "./parts";
import { ModelRow } from "./ModelRow";
import { formatCount, GEAR_LABELS, SIZE_LABELS } from "./labels";
import s from "./t3k.module.css";

/** Carries the tone (and retry) it answered, so loading stays derived. */
interface Files {
  key: string;
  models: T3kModel[];
  error: string | null;
}

export interface ToneDetailProps {
  tone: T3kTone;
  onBack(): void;
  onClose(): void;
  onToggleFavorite(tone: T3kTone): void;
}

export function ToneDetail({
  tone,
  onBack,
  onClose,
  onToggleFavorite,
}: ToneDetailProps) {
  const [files, setFiles] = useState<Files | null>(null);
  const [attempt, setAttempt] = useState(0);
  const key = `${tone.id}:${attempt}`;
  const answered = files?.key === key ? files : null;
  const inChain = useStore((st) => {
    const paths = st.t3kDownloaded[tone.id];
    if (!paths) return false;
    return paths.some((p) => p === st.model?.path || p === st.ir?.path);
  });

  useEffect(() => {
    let cancelled = false;
    void bridge.t3kListModels(tone.id).then((res) => {
      if (cancelled) return;
      setFiles({ key, models: res.models ?? [], error: res.error ?? null });
    });
    return () => {
      cancelled = true;
    };
  }, [tone.id, key]);

  const chips = [
    ...tone.makes,
    ...tone.tags,
    ...tone.sizes.map((size) => SIZE_LABELS[size] ?? size),
  ];

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
            {inChain && <span className={s.bannerLoaded}>In chain</span>}
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
          <HeartButton tone={tone} onToggle={onToggleFavorite} />
        </div>

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

        <div className={s.sectionHead}>
          <span>Files</span>
          <span className={s.sectionCount}>
            {answered?.models.length ?? tone.modelsCount}
          </span>
        </div>

        {answered === null ? (
          <ul className={s.fileList}>
            {[0, 1].map((i) => (
              <li key={i} className={s.file}>
                <Skeleton width="60%" height={11} />
              </li>
            ))}
          </ul>
        ) : answered.error ? (
          <EmptyState
            className={s.filesEmpty}
            message="No files to show"
            hint={answered.error}
            action={
              <Button size="sm" onClick={() => setAttempt((a) => a + 1)}>
                Try again
              </Button>
            }
          />
        ) : (
          <ul className={s.fileList}>
            {answered.models.map((model) => (
              <ModelRow key={model.id} tone={tone} model={model} />
            ))}
          </ul>
        )}
      </div>
    </>
  );
}
