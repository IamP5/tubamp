/** PROTOTYPE — THROWAWAY. Variant C: Side drawer. */
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { AnimatePresence, motion, useReducedMotion } from "motion/react";
import {
  Button,
  CheckIcon,
  ChevronDownIcon,
  ChevronLeftIcon,
  CloseIcon,
  DownloadIcon,
  EmptyState,
  IconButton,
  Segmented,
  Skeleton,
  Spinner,
  cx,
} from "../../components";
import { duration, ease, noMotion, spring, staggerFor, tween } from "../../theme/motion";
import { setVariantParam } from "./PrototypeSwitcher";
import {
  fetchCatalog,
  formatCount,
  GEAR_LABELS,
  SORT_LABELS,
  useProtoStore,
} from "./catalog";
import type {
  ProtoKind,
  ProtoModel,
  ProtoQuery,
  ProtoSort,
  ProtoTone,
} from "./catalog";
import v from "./variantc.module.css";

/* ── seeded favourites: lets the row counts move optimistically by ±1 ── */
const SEEDED_FAVORITES = new Set(useProtoStore.getState().favorites);

const KIND_OPTIONS: { value: ProtoKind; label: string }[] = [
  { value: "models", label: "Models" },
  { value: "irs", label: "IRs" },
];
const SORTS: ProtoSort[] = ["trending", "newest", "downloads"];
const GEAR_CHIPS = ["amp", "amp-cab", "pedal"];
const SIZE_CHIPS: { value: string; label: string }[] = [
  { value: "lite", label: "Lite" },
  { value: "feather", label: "Feather" },
  { value: "nano", label: "Nano" },
];

const SIZE_LABELS: Record<string, string> = {
  standard: "Standard",
  lite: "Lite",
  feather: "Feather",
  nano: "Nano",
};

/* ────────────────────────────── tiny glyphs ────────────────────────────── */

function HeartIcon({ filled, size = 14 }: { filled?: boolean; size?: number }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill={filled ? "currentColor" : "none"}
      stroke="currentColor"
      strokeWidth={1.5}
      strokeLinejoin="round"
      aria-hidden="true"
    >
      <path d="M8 13.3C8 13.3 2.5 10 2.5 6.4A2.9 2.9 0 0 1 8 5a2.9 2.9 0 0 1 5.5 1.4c0 3.6-5.5 6.9-5.5 6.9z" />
    </svg>
  );
}

function SearchIcon({ size = 13 }: { size?: number }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 16 16"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.6}
      strokeLinecap="round"
      aria-hidden="true"
    >
      <circle cx="7" cy="7" r="4.2" />
      <path d="M10.3 10.3 13.5 13.5" />
    </svg>
  );
}

/** The persistent TONE3000 attribution mark. */
function T3kMark({ compact }: { compact?: boolean }) {
  return (
    <span className={cx(v.mark, compact && v.markCompact)}>
      <span className={v.markGlyph} aria-hidden="true">
        <i />
        <i />
        <i />
      </span>
      {compact ? "TONE3000" : "Powered by TONE3000"}
    </span>
  );
}

/* ──────────────────────────── sort dropdown ──────────────────────────── */

function SortMenu({
  value,
  onChange,
}: {
  value: ProtoSort;
  onChange(next: ProtoSort): void;
}) {
  const [open, setOpen] = useState(false);
  const wrapRef = useRef<HTMLDivElement>(null);
  const reduced = useReducedMotion() ?? false;

  useEffect(() => {
    if (!open) return;
    const onDown = (e: MouseEvent) => {
      if (!wrapRef.current?.contains(e.target as Node)) setOpen(false);
    };
    window.addEventListener("mousedown", onDown);
    return () => window.removeEventListener("mousedown", onDown);
  }, [open]);

  return (
    <div className={v.sortWrap} ref={wrapRef}>
      <button
        type="button"
        className={cx(v.sortButton, open && v.sortButtonOpen)}
        onClick={() => setOpen((o) => !o)}
      >
        <span className={v.sortLabel}>{SORT_LABELS[value]}</span>
        <ChevronDownIcon size={12} />
      </button>
      <AnimatePresence>
        {open && (
          <motion.div
            className={v.sortMenu}
            initial={reduced ? false : { opacity: 0, y: -4, scale: 0.98 }}
            animate={{ opacity: 1, y: 0, scale: 1 }}
            exit={reduced ? { opacity: 0 } : { opacity: 0, y: -4, scale: 0.98 }}
            transition={reduced ? noMotion : tween.micro}
          >
            {SORTS.map((s) => (
              <button
                key={s}
                type="button"
                className={cx(v.sortItem, s === value && v.sortItemActive)}
                onClick={() => {
                  onChange(s);
                  setOpen(false);
                }}
              >
                <span>{SORT_LABELS[s]}</span>
                {s === value && <CheckIcon size={12} />}
              </button>
            ))}
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  );
}

/* ─────────────────────────── shared tone bits ─────────────────────────── */

const artStyle = (tone: ProtoTone) => ({
  backgroundImage: `linear-gradient(135deg, ${tone.art[0]}, ${tone.art[1]})`,
});

function Avatar({ tone, size = 14 }: { tone: ProtoTone; size?: number }) {
  return (
    <span
      className={v.avatar}
      style={{
        width: size,
        height: size,
        background: `hsl(${tone.creator.avatarHue} 40% 45%)`,
        fontSize: Math.round(size * 0.58),
      }}
      aria-hidden="true"
    >
      {tone.creator.username[0].toUpperCase()}
    </span>
  );
}

function FormatBadge({ tone }: { tone: ProtoTone }) {
  return (
    <span className={cx(v.badge, tone.format === "ir" && v.badgeIr)}>
      {tone.format === "ir" ? "IR" : "A2"}
    </span>
  );
}

function HeartButton({
  tone,
  showCount,
}: {
  tone: ProtoTone;
  showCount?: boolean;
}) {
  const fav = useProtoStore((st) => st.favorites.has(tone.id));
  const toggle = useProtoStore((st) => st.toggleFavorite);
  const count =
    tone.favoritesCount +
    (fav ? 1 : 0) -
    (SEEDED_FAVORITES.has(tone.id) ? 1 : 0);

  return (
    <motion.button
      type="button"
      className={cx(v.heart, fav && v.heartOn)}
      aria-label={fav ? "Remove from favorites" : "Add to favorites"}
      aria-pressed={fav}
      whileTap={{ scale: 0.86 }}
      transition={spring.micro}
      onClick={(e) => {
        e.stopPropagation();
        toggle(tone.id);
      }}
    >
      <HeartIcon filled={fav} />
      {showCount && <span className={v.heartCount}>{formatCount(count)}</span>}
    </motion.button>
  );
}

/* ──────────────────────────── level 1: list ──────────────────────────── */

function ToneRow({
  tone,
  index,
  reduced,
  onOpen,
}: {
  tone: ProtoTone;
  index: number;
  reduced: boolean;
  onOpen(tone: ProtoTone): void;
}) {
  const loadedHere = useProtoStore((st) => st.loaded?.toneId === tone.id);

  return (
    <motion.li
      layout
      className={v.row}
      initial={reduced ? false : { opacity: 0, x: 14 }}
      animate={{ opacity: 1, x: 0 }}
      exit={{ opacity: 0, x: -10 }}
      transition={
        reduced
          ? noMotion
          : {
              ...spring.reflow,
              delay: index * staggerFor(12),
            }
      }
    >
      <div
        className={v.rowHit}
        role="button"
        tabIndex={0}
        onClick={() => onOpen(tone)}
        onKeyDown={(e) => {
          if (e.key === "Enter" || e.key === " ") onOpen(tone);
        }}
      >
        <span className={v.rowArt} style={artStyle(tone)}>
          {loadedHere && <span className={v.artLoadedDot} />}
        </span>

        <span className={v.rowBody}>
          <span className={v.rowTitle}>{tone.title}</span>
          <span className={v.rowMeta}>
            <Avatar tone={tone} />
            <span className={v.rowUser}>{tone.creator.username}</span>
            <span className={v.dot}>·</span>
            <span className={v.rowGear}>{GEAR_LABELS[tone.gear]}</span>
            <FormatBadge tone={tone} />
          </span>
        </span>

        <span className={v.rowTrail}>
          <HeartButton tone={tone} showCount />
          <span className={v.rowDownloads}>
            <DownloadIcon size={11} />
            {formatCount(tone.downloadsCount)}
          </span>
        </span>
      </div>
    </motion.li>
  );
}

function RowSkeleton() {
  return (
    <li className={v.row}>
      <div className={v.rowHit}>
        <Skeleton width={44} height={44} radius="var(--radius-sm)" />
        <span className={v.rowBody}>
          <Skeleton width="72%" height={11} />
          <Skeleton width="46%" height={9} style={{ marginTop: 7 }} />
        </span>
      </div>
    </li>
  );
}

/* ─────────────────────────── level 2: detail ─────────────────────────── */

function FileRow({ tone, model }: { tone: ProtoTone; model: ProtoModel }) {
  const dl = useProtoStore((st) => st.downloads[model.id]);
  const isLoaded = useProtoStore((st) => st.loaded?.modelId === model.id);
  const downloadAndLoad = useProtoStore((st) => st.downloadAndLoad);
  const busy = !!dl && !dl.done;

  return (
    <li className={cx(v.file, isLoaded && v.fileLoaded)}>
      <span className={v.fileMain}>
        <span className={v.fileName}>{model.name}</span>
        <span className={v.fileMeta}>
          <span className={v.sizeTag}>{SIZE_LABELS[model.size]}</span>
          <span className={v.fileKind}>
            {model.architecture ? `NAM · arch ${model.architecture}` : "WAV · IR"}
          </span>
        </span>
      </span>

      <span className={v.fileAction}>
        {isLoaded ? (
          <span className={v.loadedTag}>
            <CheckIcon size={12} />
            Loaded
          </span>
        ) : busy ? (
          <span className={v.progressWrap}>
            <span className={v.progressTrack}>
              <motion.span
                className={v.progressFill}
                animate={{ scaleX: Math.max(0.04, dl.progress) }}
                transition={tween.micro}
              />
            </span>
            <span className={v.progressPct}>
              {Math.round(dl.progress * 100)}%
            </span>
          </span>
        ) : (
          <Button
            size="sm"
            variant={dl?.done ? "default" : "primary"}
            icon={dl?.done ? undefined : <DownloadIcon size={13} />}
            onClick={() => downloadAndLoad(tone, model)}
          >
            {dl?.done ? "Load" : "Get"}
          </Button>
        )}
      </span>
    </li>
  );
}

function DetailLevel({
  tone,
  onBack,
  onClose,
}: {
  tone: ProtoTone;
  onBack(): void;
  onClose(): void;
}) {
  const loadedHere = useProtoStore((st) => st.loaded?.toneId === tone.id);
  const chips = [
    ...tone.makes,
    ...tone.tags,
    ...tone.sizes.map((s) => SIZE_LABELS[s]),
  ];

  return (
    <>
      <header className={v.head}>
        <IconButton aria-label="Back to browse" onClick={onBack} size="sm">
          <ChevronLeftIcon size={16} />
        </IconButton>
        <span className={v.headTitle}>{tone.title}</span>
        <IconButton aria-label="Close browser" onClick={onClose} size="sm">
          <CloseIcon size={14} />
        </IconButton>
      </header>

      <div className={v.scroll}>
        <div className={v.banner} style={artStyle(tone)}>
          <span className={v.bannerScrim} />
          <span className={v.bannerTags}>
            <span className={v.gearTag}>{GEAR_LABELS[tone.gear]}</span>
            <FormatBadge tone={tone} />
            {loadedHere && <span className={v.bannerLoaded}>In chain</span>}
          </span>
        </div>

        <div className={v.creatorRow}>
          <Avatar tone={tone} size={22} />
          <span className={v.creatorCol}>
            <span className={v.creatorName}>{tone.creator.username}</span>
            <span className={v.creatorStats}>
              <DownloadIcon size={11} />
              {formatCount(tone.downloadsCount)}
              <span className={v.dot}>·</span>
              <HeartIcon size={11} filled />
              {formatCount(tone.favoritesCount)}
            </span>
          </span>
          <HeartButton tone={tone} />
        </div>

        <p className={v.description}>{tone.description}</p>

        <div className={v.tagRow}>
          {chips.map((c) => (
            <span key={c} className={v.tag}>
              {c}
            </span>
          ))}
        </div>

        <div className={v.sectionHead}>
          <span>Files</span>
          <span className={v.sectionCount}>{tone.models.length}</span>
        </div>
        <ul className={v.fileList}>
          {tone.models.map((m) => (
            <FileRow key={m.id} tone={tone} model={m} />
          ))}
        </ul>
      </div>
    </>
  );
}

/* ────────────────────────────── the drawer ────────────────────────────── */

export function VariantC() {
  const reduced = useReducedMotion() ?? false;
  const [query, setQuery] = useState<ProtoQuery>({
    kind: "models",
    sort: "trending",
    query: "",
    gear: null,
    size: null,
    favoritesOnly: false,
  });
  const [search, setSearch] = useState("");
  /* Result carries the query it answered, so `loading` is derived, not stored. */
  const [result, setResult] = useState<{
    q: ProtoQuery;
    rows: ProtoTone[];
  } | null>(null);
  const [detail, setDetail] = useState<ProtoTone | null>(null);

  const loading = result === null || result.q !== query;

  const favorites = useProtoStore((st) => st.favorites);
  const loaded = useProtoStore((st) => st.loaded);

  const update = useCallback(
    (patch: Partial<ProtoQuery>) => setQuery((q) => ({ ...q, ...patch })),
    [],
  );

  /* Debounced search — optional sugar, so it never gates the fetch. */
  useEffect(() => {
    const id = setTimeout(
      () => setQuery((q) => (q.query === search ? q : { ...q, query: search })),
      200,
    );
    return () => clearTimeout(id);
  }, [search]);

  /*
    Favourites are read once per fetch instead of being a dependency: hearting a
    tone must not re-trigger the fake network. The favourites-only view filters
    the rendered rows below, so the toggle still feels instant.
  */
  useEffect(() => {
    let cancelled = false;
    void fetchCatalog(query, useProtoStore.getState().favorites).then((res) => {
      if (!cancelled) setResult({ q: query, rows: res });
    });
    return () => {
      cancelled = true;
    };
  }, [query]);

  const visible = useMemo(() => {
    const list = result?.rows ?? [];
    return query.favoritesOnly ? list.filter((t) => favorites.has(t.id)) : list;
  }, [result, query.favoritesOnly, favorites]);

  const filtered = query.gear !== null || query.size !== null || query.favoritesOnly;
  const close = useCallback(() => setVariantParam(null), []);

  return (
    <motion.aside
      className={v.drawer}
      initial={reduced ? false : { x: 400 }}
      animate={{ x: 0 }}
      transition={reduced ? noMotion : { ...spring.reflow, mass: 1.1 }}
      aria-label="TONE3000 browser"
    >
      <div className={v.levels}>
        {/* ── level 1 · browse ── */}
        <motion.div
          className={v.level}
          animate={{ x: detail ? -76 : 0, opacity: detail ? 0.35 : 1 }}
          transition={reduced ? noMotion : spring.ui}
          /* `inert` (not aria-hidden) — the pushed-away level may still hold
             focus, which aria-hidden is not allowed to cover. */
          inert={detail !== null}
          style={{ pointerEvents: detail ? "none" : "auto" }}
        >
          <header className={v.head}>
            <T3kMark compact />
            <span className={v.headSpacer} />
            <IconButton aria-label="Close browser" onClick={close} size="sm">
              <CloseIcon size={14} />
            </IconButton>
          </header>

          <div className={v.controls}>
            <div className={v.controlRow}>
              <Segmented
                options={KIND_OPTIONS}
                value={query.kind}
                onChange={(kind) => update({ kind, gear: null, size: null })}
                aria-label="Catalog kind"
              />
              <SortMenu
                value={query.sort}
                onChange={(sort) => update({ sort })}
              />
            </div>

            <label className={v.searchRow}>
              <SearchIcon />
              <input
                className={v.searchInput}
                value={search}
                placeholder="Search — optional"
                onChange={(e) => setSearch(e.target.value)}
              />
              {search !== "" && (
                <button
                  type="button"
                  className={v.searchClear}
                  aria-label="Clear search"
                  onClick={() => setSearch("")}
                >
                  <CloseIcon size={11} />
                </button>
              )}
            </label>

            <div className={v.chips}>
              <button
                type="button"
                className={cx(
                  v.chip,
                  v.chipFav,
                  query.favoritesOnly && v.chipOn,
                )}
                onClick={() => update({ favoritesOnly: !query.favoritesOnly })}
              >
                <HeartIcon filled={query.favoritesOnly} size={11} />
                Favorites
              </button>

              {query.kind === "models" ? (
                GEAR_CHIPS.map((g) => (
                  <button
                    key={g}
                    type="button"
                    className={cx(v.chip, query.gear === g && v.chipOn)}
                    onClick={() =>
                      update({ gear: query.gear === g ? null : g })
                    }
                  >
                    {GEAR_LABELS[g]}
                  </button>
                ))
              ) : (
                <span className={v.chipStatic}>{GEAR_LABELS.cab}</span>
              )}

              {query.kind === "models" &&
                SIZE_CHIPS.map((s) => (
                  <button
                    key={s.value}
                    type="button"
                    className={cx(
                      v.chip,
                      v.chipSize,
                      query.size === s.value && v.chipOn,
                    )}
                    onClick={() =>
                      update({ size: query.size === s.value ? null : s.value })
                    }
                  >
                    {s.label}
                  </button>
                ))}
            </div>

            <div className={v.countLine}>
              <span>
                {visible.length} {query.kind === "irs" ? "IRs" : "models"} ·{" "}
                {SORT_LABELS[query.sort].toLowerCase()}
              </span>
              <span className={v.countRight}>
                {loading && <Spinner size={11} />}
                {filtered && (
                  <button
                    type="button"
                    className={v.clearButton}
                    onClick={() =>
                      update({ gear: null, size: null, favoritesOnly: false })
                    }
                  >
                    Clear
                  </button>
                )}
              </span>
            </div>
          </div>

          <div className={v.scroll}>
            {result === null ? (
              <ul className={v.list}>
                {[0, 1, 2, 3, 4, 5].map((i) => (
                  <RowSkeleton key={i} />
                ))}
              </ul>
            ) : visible.length === 0 ? (
              <EmptyState
                className={v.empty}
                message="Nothing here yet"
                hint={
                  query.favoritesOnly
                    ? "Heart a tone to pin it to this list."
                    : "Loosen a filter to see more captures."
                }
                action={
                  filtered ? (
                    <Button
                      size="sm"
                      onClick={() =>
                        update({ gear: null, size: null, favoritesOnly: false })
                      }
                    >
                      Reset filters
                    </Button>
                  ) : undefined
                }
              />
            ) : (
              <ul className={v.list}>
                <AnimatePresence initial={false}>
                  {visible.map((tone, i) => (
                    <ToneRow
                      key={tone.id}
                      tone={tone}
                      index={i}
                      reduced={reduced}
                      onOpen={setDetail}
                    />
                  ))}
                </AnimatePresence>
              </ul>
            )}
          </div>
        </motion.div>

        {/* ── level 2 · tone detail ── */}
        <AnimatePresence>
          {detail && (
            <motion.div
              className={cx(v.level, v.levelTop)}
              initial={reduced ? { opacity: 0 } : { x: "100%" }}
              animate={reduced ? { opacity: 1 } : { x: 0 }}
              exit={reduced ? { opacity: 0 } : { x: "100%" }}
              transition={
                reduced
                  ? noMotion
                  : { ...spring.ui, restDelta: 0.5 }
              }
            >
              <DetailLevel
                tone={detail}
                onBack={() => setDetail(null)}
                onClose={close}
              />
            </motion.div>
          )}
        </AnimatePresence>
      </div>

      {/* ── persistent status bar ── */}
      <footer className={v.dockbar}>
        <div className={v.statusLine}>
          <span
            className={cx(v.statusDot, loaded && v.statusDotOn)}
            aria-hidden="true"
          />
          <AnimatePresence mode="wait" initial={false}>
            <motion.span
              key={loaded?.modelId ?? "none"}
              className={v.statusText}
              initial={reduced ? false : { opacity: 0, y: 4 }}
              animate={{ opacity: 1, y: 0 }}
              exit={reduced ? { opacity: 0 } : { opacity: 0, y: -4 }}
              transition={
                reduced ? noMotion : { duration: duration.fast, ease: ease.out }
              }
            >
              {loaded ? loaded.name : "No capture loaded"}
            </motion.span>
          </AnimatePresence>
          {loaded && (
            <span className={v.statusKind}>
              {loaded.kind === "wav" ? "IR" : "AMP"}
            </span>
          )}
        </div>
        <T3kMark />
      </footer>
    </motion.aside>
  );
}
