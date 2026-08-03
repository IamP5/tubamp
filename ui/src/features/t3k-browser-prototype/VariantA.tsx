/** PROTOTYPE — THROWAWAY. Variant A: Store takeover. */
import { useCallback, useEffect, useMemo, useState } from "react";
import { AnimatePresence, motion, useReducedMotion } from "motion/react";
import {
  Button,
  CheckCircleIcon,
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
import {
  duration,
  ease,
  noMotion,
  spring,
  staggerFor,
  tween,
} from "../../theme/motion";
import { setVariantParam } from "./PrototypeSwitcher";
import {
  CATALOG,
  GEAR_LABELS,
  SORT_LABELS,
  fetchCatalog,
  formatCount,
  useProtoStore,
  type ProtoKind,
  type ProtoModel,
  type ProtoQuery,
  type ProtoSort,
  type ProtoTone,
} from "./catalog";
import v from "./varianta.module.css";

/* ───────────────────────────── local glyphs ───────────────────────────── */

const HeartIcon = ({ size = 16, filled }: { size?: number; filled?: boolean }) => (
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
    <path d="M8 13.2S2.6 10 2.6 6.2a2.9 2.9 0 0 1 5.4-1.6 2.9 2.9 0 0 1 5.4 1.6C13.4 10 8 13.2 8 13.2z" />
  </svg>
);

const SearchIcon = ({ size = 14 }: { size?: number }) => (
  <svg
    width={size}
    height={size}
    viewBox="0 0 16 16"
    fill="none"
    stroke="currentColor"
    strokeWidth={1.5}
    strokeLinecap="round"
    aria-hidden="true"
  >
    <circle cx="7" cy="7" r="4.2" />
    <path d="M10.2 10.2L13.5 13.5" />
  </svg>
);

/* ────────────────────────────── small bits ────────────────────────────── */

const SIZE_LABELS: Record<string, string> = {
  standard: "Standard",
  lite: "Lite",
  feather: "Feather",
  nano: "Nano",
};

/** Fabricated but plausible file weights — the mock data has no byte counts. */
const SIZE_BYTES: Record<string, string> = {
  standard: "1.2 MB",
  lite: "480 KB",
  feather: "184 KB",
  nano: "62 KB",
};

const KIND_OPTIONS = [
  { value: "models" as const, label: "Models" },
  { value: "irs" as const, label: "IRs" },
];

const SORTS: ProtoSort[] = ["trending", "newest", "downloads"];
const GEARS = ["amp", "amp-cab", "pedal"];
const SIZES = ["standard", "lite", "feather", "nano"];

function Avatar({ tone, size = 16 }: { tone: ProtoTone; size?: number }) {
  return (
    <span
      className={v.avatar}
      style={{
        width: size,
        height: size,
        background: `hsl(${tone.creator.avatarHue} 40% 45%)`,
        fontSize: Math.round(size * 0.55),
      }}
      aria-hidden="true"
    >
      {tone.creator.username.charAt(0).toUpperCase()}
    </span>
  );
}

function Art({ tone, className }: { tone: ProtoTone; className?: string }) {
  return (
    <span
      className={cx(v.art, className)}
      style={{
        backgroundImage: `linear-gradient(135deg, ${tone.art[0]}, ${tone.art[1]})`,
      }}
    >
      <span className={v.artTexture} />
      <span className={v.artVignette} />
    </span>
  );
}

function Wordmark({ small }: { small?: boolean }) {
  return (
    <span className={cx(v.wordmark, small && v.wordmarkSmall)}>
      TONE<span className={v.wordmarkAccent}>3000</span>
    </span>
  );
}

function FilterRow({
  label,
  count,
  active,
  pillId,
  icon,
  onClick,
}: {
  label: string;
  count?: number;
  active: boolean;
  pillId: string;
  icon?: React.ReactNode;
  onClick(): void;
}) {
  return (
    <button
      type="button"
      className={cx(v.row, active && v.rowActive)}
      onClick={onClick}
    >
      {active && (
        <motion.span
          layoutId={pillId}
          className={v.rowPill}
          transition={spring.ui}
        />
      )}
      {icon && <span className={v.rowIcon}>{icon}</span>}
      <span className={v.rowLabel}>{label}</span>
      {count !== undefined && <span className={v.rowCount}>{count}</span>}
    </button>
  );
}

function HeartButton({
  tone,
  className,
}: {
  tone: ProtoTone;
  className?: string;
}) {
  const favorite = useProtoStore((st) => st.favorites.has(tone.id));
  const toggleFavorite = useProtoStore((st) => st.toggleFavorite);
  return (
    <motion.button
      type="button"
      aria-label={favorite ? "Remove from favorites" : "Add to favorites"}
      aria-pressed={favorite}
      className={cx(v.heart, favorite && v.heartOn, className)}
      whileTap={{ scale: 0.86 }}
      transition={spring.micro}
      onClick={(e) => {
        e.stopPropagation();
        toggleFavorite(tone.id);
      }}
    >
      <HeartIcon size={14} filled={favorite} />
    </motion.button>
  );
}

/* ─────────────────────────────── the card ─────────────────────────────── */

const cardVariants = {
  hidden: { opacity: 0, y: 10 },
  visible: { opacity: 1, y: 0, transition: tween.enter },
};

function ToneCard({ tone, onOpen }: { tone: ProtoTone; onOpen(): void }) {
  const loadedHere = useProtoStore((st) => st.loaded?.toneId === tone.id);
  const favorite = useProtoStore((st) => st.favorites.has(tone.id));

  return (
    <motion.div
      variants={cardVariants}
      className={cx(v.card, favorite && v.cardFav)}
      whileHover={{ y: -3 }}
      transition={spring.micro}
      onClick={onOpen}
      role="button"
      tabIndex={0}
      onKeyDown={(e) => {
        if (e.key === "Enter" || e.key === " ") onOpen();
      }}
    >
      <div className={v.artWrap}>
        <Art tone={tone} />
        <span className={v.artBadge}>{tone.format === "nam" ? "A2" : "IR"}</span>
        {loadedHere && (
          <span className={v.artLoaded}>
            <CheckCircleIcon size={11} />
            Loaded
          </span>
        )}
        <HeartButton tone={tone} className={v.artHeart} />
      </div>

      <div className={v.cardBody}>
        <span className={v.cardTitle}>{tone.title}</span>
        <span className={v.creator}>
          <Avatar tone={tone} size={15} />
          {tone.creator.username}
        </span>
        <span className={v.cardMeta}>
          <span className={v.gearTag}>{GEAR_LABELS[tone.gear]}</span>
          <span className={v.metaStat}>
            <DownloadIcon size={11} />
            {formatCount(tone.downloadsCount)}
          </span>
          <span className={v.metaStat}>
            <HeartIcon size={11} />
            {formatCount(tone.favoritesCount)}
          </span>
        </span>
      </div>
    </motion.div>
  );
}

function CardSkeleton() {
  return (
    <div className={v.card}>
      <Skeleton height={128} radius="var(--radius-md)" />
      <div className={v.cardBody}>
        <Skeleton height={11} width="82%" />
        <Skeleton height={9} width="52%" />
        <Skeleton height={9} width="66%" />
      </div>
    </div>
  );
}

/* ───────────────────────────── drawer file row ────────────────────────── */

function FileRow({ tone, model }: { tone: ProtoTone; model: ProtoModel }) {
  const dl = useProtoStore((st) => st.downloads[model.id]);
  const isLoaded = useProtoStore((st) => st.loaded?.modelId === model.id);
  const downloadAndLoad = useProtoStore((st) => st.downloadAndLoad);
  const busy = dl !== undefined && !dl.done;

  return (
    <div className={cx(v.file, isLoaded && v.fileLoaded)}>
      <span className={v.fileBadge}>
        {model.architecture === "2" ? "A2" : "IR"}
      </span>
      <span className={v.fileMain}>
        <span className={v.fileName}>{model.name}</span>
        <span className={v.fileMeta}>
          {model.fileKind === "nam"
            ? `${SIZE_LABELS[model.size]} · ${SIZE_BYTES[model.size]} · .nam`
            : `Impulse response · 96 KB · .wav`}
        </span>
      </span>

      {busy ? (
        <span className={v.pct}>{Math.round(dl.progress * 100)}%</span>
      ) : isLoaded ? (
        <span className={v.loadedPill}>
          <CheckCircleIcon size={12} />
          Loaded
        </span>
      ) : (
        <Button
          size="sm"
          variant={dl?.done ? "ghost" : "default"}
          icon={dl?.done ? undefined : <DownloadIcon size={14} />}
          onClick={() => downloadAndLoad(tone, model)}
        >
          {dl?.done ? "Load" : "Get"}
        </Button>
      )}

      {busy && (
        <span className={v.progressTrack}>
          <motion.span
            className={v.progressFill}
            initial={{ scaleX: 0 }}
            animate={{ scaleX: dl.progress }}
            transition={tween.micro}
          />
        </span>
      )}
    </div>
  );
}

/* ─────────────────────────────── the shell ────────────────────────────── */

const EMPTY_QUERY: ProtoQuery = {
  kind: "models",
  sort: "trending",
  query: "",
  gear: null,
  size: null,
  favoritesOnly: false,
};

export function VariantA() {
  const reduced = useReducedMotion() ?? false;
  const [q, setQ] = useState<ProtoQuery>(EMPTY_QUERY);
  const [rows, setRows] = useState<ProtoTone[] | null>(null);
  const [loading, setLoading] = useState(true);
  const [selectedId, setSelectedId] = useState<number | null>(null);

  const favorites = useProtoStore((st) => st.favorites);

  /**
   * The busy flag is raised where the query actually changes, not inside the
   * fetch effect — a synchronous setState in an effect body is a cascading
   * render (react-hooks/set-state-in-effect). A favourite toggle also re-fetches
   * (the favourites-only view depends on it) but deliberately stays silent.
   */
  const applyQuery = useCallback((next: ProtoQuery) => {
    setLoading(true);
    setQ(next);
  }, []);
  const patch = useCallback(
    (next: Partial<ProtoQuery>) => {
      setLoading(true);
      setQ((prev) => ({ ...prev, ...next }));
    },
    [],
  );

  useEffect(() => {
    let cancelled = false;
    void fetchCatalog(q, favorites).then((res) => {
      if (cancelled) return;
      setRows(res);
      setLoading(false);
    });
    return () => {
      cancelled = true;
    };
  }, [q, favorites]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== "Escape") return;
      if (selectedId !== null) setSelectedId(null);
      else setVariantParam(null);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [selectedId]);

  /** Facet counts, computed over the current kind only (classic store sidebar). */
  const facets = useMemo(() => {
    const pool = CATALOG.filter((t) =>
      q.kind === "irs" ? t.format === "ir" : t.format === "nam",
    );
    const gear: Record<string, number> = {};
    const size: Record<string, number> = {};
    for (const t of pool) {
      gear[t.gear] = (gear[t.gear] ?? 0) + 1;
      for (const s of new Set(t.sizes)) size[s] = (size[s] ?? 0) + 1;
    }
    return { total: pool.length, gear, size };
  }, [q.kind]);

  const favoriteCount = useMemo(
    () =>
      CATALOG.filter(
        (t) =>
          favorites.has(t.id) &&
          (q.kind === "irs" ? t.format === "ir" : t.format === "nam"),
      ).length,
    [favorites, q.kind],
  );

  const selected = useMemo(
    () => CATALOG.find((t) => t.id === selectedId) ?? null,
    [selectedId],
  );

  const showSkeletons = loading && rows === null;
  const list = rows ?? [];
  const gridKey = `${q.kind}|${q.sort}|${q.gear}|${q.size}|${q.favoritesOnly}|${q.query}`;

  return (
    <motion.div
      className={v.root}
      initial={reduced ? false : { opacity: 0, scale: 0.99 }}
      animate={{ opacity: 1, scale: 1 }}
      transition={{ duration: duration.slow, ease: ease.out }}
    >
      {/* ───────── top bar ───────── */}
      <header className={v.topbar}>
        <IconButton aria-label="Back to the amp" onClick={() => setVariantParam(null)}>
          <ChevronLeftIcon />
        </IconButton>
        <Wordmark />
        <span className={v.topDivider} />
        <Segmented<ProtoKind>
          aria-label="Browse models or IRs"
          options={KIND_OPTIONS}
          value={q.kind}
          onChange={(kind) => patch({ kind, gear: null, size: null })}
        />
        <span className={v.spacer} />
        <label className={v.search}>
          <SearchIcon />
          <input
            className={v.searchInput}
            value={q.query}
            placeholder="Search tones"
            onChange={(e) => patch({ query: e.target.value })}
          />
          {q.query ? (
            <IconButton
              size="sm"
              aria-label="Clear search"
              onClick={() => patch({ query: "" })}
            >
              <CloseIcon size={12} />
            </IconButton>
          ) : (
            <span className={v.searchHint}>optional</span>
          )}
        </label>
      </header>

      <div className={v.body}>
        {/* ───────── sidebar ───────── */}
        <motion.aside
          className={v.sidebar}
          initial={reduced ? false : { opacity: 0, x: -8 }}
          animate={{ opacity: 1, x: 0 }}
          transition={{ duration: duration.base, ease: ease.out, delay: 0.04 }}
        >
          <div className={v.group}>
            <span className={v.groupTitle}>Sort</span>
            {SORTS.map((s) => (
              <FilterRow
                key={s}
                pillId="sb-sort"
                label={SORT_LABELS[s]}
                active={q.sort === s}
                onClick={() => patch({ sort: s })}
              />
            ))}
          </div>

          {q.kind === "models" ? (
            <>
              <div className={v.group}>
                <span className={v.groupTitle}>Gear</span>
                <FilterRow
                  pillId="sb-gear"
                  label="All gear"
                  count={facets.total}
                  active={q.gear === null}
                  onClick={() => patch({ gear: null })}
                />
                {GEARS.map((g) => (
                  <FilterRow
                    key={g}
                    pillId="sb-gear"
                    label={GEAR_LABELS[g]}
                    count={facets.gear[g] ?? 0}
                    active={q.gear === g}
                    onClick={() => patch({ gear: g })}
                  />
                ))}
              </div>

              <div className={v.group}>
                <span className={v.groupTitle}>Model size</span>
                <FilterRow
                  pillId="sb-size"
                  label="Any size"
                  active={q.size === null}
                  onClick={() => patch({ size: null })}
                />
                {SIZES.map((s) => (
                  <FilterRow
                    key={s}
                    pillId="sb-size"
                    label={SIZE_LABELS[s]}
                    count={facets.size[s] ?? 0}
                    active={q.size === s}
                    onClick={() => patch({ size: s })}
                  />
                ))}
              </div>
            </>
          ) : (
            <div className={v.group}>
              <span className={v.groupTitle}>Cab IRs</span>
              <p className={v.groupNote}>
                Every IR ships as a single standard-rate .wav — no size or gear
                split. Sort and favorites still apply.
              </p>
            </div>
          )}

          <div className={v.group}>
            <span className={v.groupTitle}>Library</span>
            <FilterRow
              pillId="sb-fav"
              label="Favorites only"
              icon={<HeartIcon size={12} filled={q.favoritesOnly} />}
              count={favoriteCount}
              active={q.favoritesOnly}
              onClick={() => patch({ favoritesOnly: !q.favoritesOnly })}
            />
          </div>

          <div className={v.sidebarFoot}>
            <span className={v.poweredBy}>Powered by</span>
            <Wordmark small />
          </div>
        </motion.aside>

        {/* ───────── main grid ───────── */}
        <div className={v.mainWrap}>
          <motion.div
            className={v.mainInner}
            animate={{ opacity: selected ? 0.42 : 1 }}
            transition={reduced ? noMotion : tween.crossfade}
          >
            <div className={v.resultBar}>
              <span className={v.resultCount}>
                {showSkeletons
                  ? "Loading catalog…"
                  : `${list.length} ${q.kind === "models" ? "capture" : "cab IR"}${
                      list.length === 1 ? "" : "s"
                    }`}
              </span>
              <span className={v.resultSort}>· {SORT_LABELS[q.sort]}</span>
              {loading && !showSkeletons && <Spinner size={13} className={v.inlineSpinner} />}
            </div>

            <div className={v.scroller}>
              {showSkeletons ? (
                <div className={v.grid}>
                  {Array.from({ length: 8 }, (_, i) => (
                    <CardSkeleton key={i} />
                  ))}
                </div>
              ) : list.length === 0 ? (
                <EmptyState
                  className={v.empty}
                  message="No tones match those filters"
                  hint="Clear a filter or switch between Models and IRs."
                  action={
                    <Button
                      size="sm"
                      onClick={() => applyQuery({ ...EMPTY_QUERY, kind: q.kind })}
                    >
                      Reset filters
                    </Button>
                  }
                />
              ) : (
                <motion.div
                  key={gridKey}
                  className={v.grid}
                  initial={reduced ? false : "hidden"}
                  animate="visible"
                  variants={{
                    visible: {
                      transition: {
                        staggerChildren: staggerFor(list.length),
                        delayChildren: 0.02,
                      },
                    },
                  }}
                >
                  {list.map((tone) => (
                    <ToneCard
                      key={tone.id}
                      tone={tone}
                      onOpen={() => setSelectedId(tone.id)}
                    />
                  ))}
                </motion.div>
              )}
            </div>
          </motion.div>

          <AnimatePresence>
            {selected && (
              <motion.button
                type="button"
                aria-label="Close details"
                className={v.scrim}
                initial={{ opacity: 0 }}
                animate={{ opacity: 1 }}
                exit={{ opacity: 0 }}
                transition={tween.micro}
                onClick={() => setSelectedId(null)}
              />
            )}
          </AnimatePresence>
        </div>

        {/* ───────── detail drawer ───────── */}
        <AnimatePresence>
          {selected && (
            <motion.aside
              key={selected.id}
              className={v.drawer}
              initial={reduced ? { x: 0 } : { x: 400 }}
              animate={{ x: 0 }}
              exit={reduced ? { opacity: 0 } : { x: 400 }}
              transition={reduced ? noMotion : spring.ui}
            >
              <div className={v.drawerArt}>
                <Art tone={selected} className={v.drawerArtFill} />
                <div className={v.drawerArtTools}>
                  <HeartButton tone={selected} className={v.drawerHeart} />
                  <IconButton
                    plate
                    aria-label="Close details"
                    onClick={() => setSelectedId(null)}
                  >
                    <CloseIcon />
                  </IconButton>
                </div>
                <div className={v.drawerArtText}>
                  <span className={v.drawerBadges}>
                    <span className={v.artBadge}>
                      {selected.format === "nam" ? "A2" : "IR"}
                    </span>
                    <span className={v.gearTag}>{GEAR_LABELS[selected.gear]}</span>
                  </span>
                  <h2 className={v.drawerTitle}>{selected.title}</h2>
                </div>
              </div>

              <div className={v.drawerScroll}>
                <div className={v.drawerCreator}>
                  <Avatar tone={selected} size={22} />
                  <span className={v.drawerCreatorName}>
                    {selected.creator.username}
                  </span>
                  <span className={v.drawerStats}>
                    <span className={v.metaStat}>
                      <DownloadIcon size={11} />
                      {formatCount(selected.downloadsCount)}
                    </span>
                    <span className={v.metaStat}>
                      <HeartIcon size={11} />
                      {formatCount(selected.favoritesCount)}
                    </span>
                  </span>
                </div>

                <p className={v.drawerDesc}>{selected.description}</p>

                <div className={v.chips}>
                  {selected.makes.map((m) => (
                    <span key={m} className={cx(v.chip, v.chipMake)}>
                      {m}
                    </span>
                  ))}
                  {selected.tags.map((t) => (
                    <span key={t} className={v.chip}>
                      {t}
                    </span>
                  ))}
                </div>

                <div className={v.filesHead}>
                  <span className={v.filesTitle}>Files</span>
                  <span className={v.filesCount}>{selected.models.length}</span>
                </div>
                <div className={v.files}>
                  {selected.models.map((model) => (
                    <FileRow key={model.id} tone={selected} model={model} />
                  ))}
                </div>

                <p className={v.drawerFoot}>
                  Downloading loads the file straight into the{" "}
                  {selected.format === "nam" ? "AMP" : "CAB"} block.
                </p>
              </div>
            </motion.aside>
          )}
        </AnimatePresence>
      </div>
    </motion.div>
  );
}
