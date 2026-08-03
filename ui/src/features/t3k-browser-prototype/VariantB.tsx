/** PROTOTYPE — THROWAWAY. Variant B: Palette modal. */
/*
 * Command-palette take on the TONE3000 browser: one centred 760×540 modal,
 * search on top, dense 56px rows below, inline accordion for the file list.
 * Zero chrome, information density over imagery — the "cmd-K" reading of the
 * problem. Mouse-only must work: the keyboard layer is pure acceleration
 * because Logic's WebView gets no keys.
 */
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type SVGProps,
} from "react";
import { AnimatePresence, motion, useReducedMotion } from "motion/react";
import {
  Button,
  EmptyState,
  Kbd,
  Skeleton,
  Spinner,
  CheckIcon,
  ChevronDownIcon,
  DownloadIcon,
  cx,
} from "../../components";
import { duration, ease, noMotion, spring, tween } from "../../theme/motion";
import { setVariantParam } from "./PrototypeSwitcher";
import {
  fetchCatalog,
  formatCount,
  useProtoStore,
  GEAR_LABELS,
  SORT_LABELS,
  type ProtoModel,
  type ProtoSort,
  type ProtoTone,
} from "./catalog";
import v from "./variantb.module.css";

/* ───────────────────────────── local glyphs ───────────────────────────── */

const SearchGlyph = (p: SVGProps<SVGSVGElement>) => (
  <svg
    viewBox="0 0 16 16"
    fill="none"
    stroke="currentColor"
    strokeWidth={1.6}
    strokeLinecap="round"
    {...p}
  >
    <circle cx="7.1" cy="7.1" r="4.4" />
    <path d="M10.4 10.4L13.5 13.5" />
  </svg>
);

const HeartGlyph = ({
  filled,
  ...p
}: SVGProps<SVGSVGElement> & { filled?: boolean }) => (
  <svg
    viewBox="0 0 16 16"
    fill={filled ? "currentColor" : "none"}
    stroke="currentColor"
    strokeWidth={1.5}
    strokeLinejoin="round"
    {...p}
  >
    <path d="M8 13.4C8 13.4 2.3 10 2.3 6.2A2.85 2.85 0 0 1 8 4.8a2.85 2.85 0 0 1 5.7 1.4c0 3.8-5.7 7.2-5.7 7.2z" />
  </svg>
);

/* ────────────────────────────── pill menu ─────────────────────────────── */

interface PillOption {
  value: string | null;
  label: string;
}

function PillMenu({
  label,
  value,
  options,
  open,
  onOpen,
  onChange,
}: {
  label: string;
  value: string | null;
  options: PillOption[];
  open: boolean;
  onOpen(): void;
  onChange(next: string | null): void;
}) {
  const current = options.find((o) => o.value === value) ?? options[0];
  return (
    <div className={v.pillWrap}>
      <button
        type="button"
        className={cx(v.pill, open && v.pillOpen)}
        onClick={onOpen}
      >
        <span className={v.pillLabel}>{label}</span>
        <span className={v.pillValue}>{current.label}</span>
        <ChevronDownIcon size={12} className={v.pillChevron} />
      </button>
      <AnimatePresence>
        {open && (
          <motion.div
            className={v.menu}
            initial={{ opacity: 0, y: -4, scale: 0.97 }}
            animate={{ opacity: 1, y: 0, scale: 1 }}
            exit={{
              opacity: 0,
              y: -4,
              scale: 0.98,
              transition: { duration: duration.fast, ease: ease.in },
            }}
            transition={{ duration: duration.fast, ease: ease.out }}
          >
            {options.map((o) => (
              <button
                key={String(o.value)}
                type="button"
                className={cx(v.menuItem, o.value === value && v.menuItemOn)}
                onClick={() => onChange(o.value)}
              >
                <span>{o.label}</span>
                {o.value === value && <CheckIcon size={12} />}
              </button>
            ))}
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  );
}

/* ─────────────────────────────── file row ─────────────────────────────── */

function FileRow({ tone, model }: { tone: ProtoTone; model: ProtoModel }) {
  const dl = useProtoStore((st) => st.downloads[model.id]);
  const loadedId = useProtoStore((st) => st.loaded?.modelId);
  const downloadAndLoad = useProtoStore((st) => st.downloadAndLoad);
  const isLoaded = loadedId === model.id;
  const busy = dl !== undefined && !dl.done;

  return (
    <div className={cx(v.file, isLoaded && v.fileLoaded)}>
      <span className={v.fileKind}>{model.fileKind === "nam" ? "A2" : "IR"}</span>
      <span className={v.fileName}>{model.name}</span>
      <span className={v.sizeChip}>{model.size}</span>
      <span className={v.fileAction}>
        {busy ? (
          <span className={v.progress}>
            <span className={v.progressTrack}>
              <motion.span
                className={v.progressFill}
                animate={{ scaleX: Math.max(0.04, dl.progress) }}
                transition={tween.instant}
              />
            </span>
            <span className={v.progressPct}>
              {Math.round(dl.progress * 100)}%
            </span>
          </span>
        ) : isLoaded ? (
          <span className={v.loadedTick}>
            <CheckIcon size={12} />
            Loaded
          </span>
        ) : (
          <Button
            size="sm"
            variant={dl?.done ? "default" : "ghost"}
            icon={dl?.done ? undefined : <DownloadIcon size={12} />}
            onClick={() => downloadAndLoad(tone, model)}
          >
            {dl?.done ? "Load" : "Get"}
          </Button>
        )}
      </span>
    </div>
  );
}

/* ──────────────────────────────── tone row ────────────────────────────── */

function ToneRow({
  tone,
  index,
  active,
  expanded,
  favCount,
  favorited,
  reduced,
  onActivate,
  onToggle,
  onFavorite,
}: {
  tone: ProtoTone;
  index: number;
  active: boolean;
  expanded: boolean;
  favCount: number;
  favorited: boolean;
  reduced: boolean;
  onActivate(): void;
  onToggle(): void;
  onFavorite(): void;
}) {
  const loadedToneId = useProtoStore((st) => st.loaded?.toneId);
  const hasLoaded = loadedToneId === tone.id;

  return (
    <div
      className={cx(v.row, active && v.rowActive, expanded && v.rowExpanded)}
      data-row={index}
    >
      <div className={v.rowHead}>
        <button
          type="button"
          className={v.rowMain}
          onMouseEnter={onActivate}
          onClick={onToggle}
        >
          <span
            className={v.thumb}
            style={{
              backgroundImage: `linear-gradient(135deg, ${tone.art[0]}, ${tone.art[1]})`,
            }}
          >
            <span className={v.thumbSheen} />
          </span>
          <span className={v.rowText}>
            <span className={v.rowTitle}>{tone.title}</span>
            <span className={v.rowSub}>
              <span
                className={v.avatar}
                style={{ background: `hsl(${tone.creator.avatarHue} 40% 45%)` }}
              >
                {tone.creator.username.charAt(0).toUpperCase()}
              </span>
              <span className={v.creator}>{tone.creator.username}</span>
              <span className={v.sep} />
              <span>{GEAR_LABELS[tone.gear]}</span>
              {hasLoaded && <span className={v.rowLoaded}>Loaded</span>}
            </span>
          </span>
          <span className={v.meta}>
            <span className={v.badge}>{tone.format === "nam" ? "A2" : "IR"}</span>
            <span className={v.count}>
              <DownloadIcon size={12} />
              {formatCount(tone.downloadsCount)}
            </span>
          </span>
          <motion.span
            className={v.caret}
            animate={{ rotate: expanded ? 0 : -90 }}
            transition={reduced ? noMotion : spring.micro}
          >
            <ChevronDownIcon size={12} />
          </motion.span>
        </button>
        <button
          type="button"
          className={cx(v.heart, favorited && v.heartOn)}
          aria-label={favorited ? "Remove from favorites" : "Add to favorites"}
          onClick={onFavorite}
        >
          <motion.span
            className={v.heartGlyph}
            animate={{ scale: favorited ? 1.12 : 1 }}
            transition={reduced ? noMotion : spring.micro}
          >
            <HeartGlyph width={13} height={13} filled={favorited} />
          </motion.span>
          {formatCount(favCount)}
        </button>
      </div>

      <AnimatePresence initial={false}>
        {expanded && (
          <motion.div
            className={v.expand}
            initial={{ height: 0, opacity: 0 }}
            animate={{ height: "auto", opacity: 1 }}
            exit={{ height: 0, opacity: 0 }}
            transition={
              reduced ? noMotion : { duration: duration.base, ease: ease.out }
            }
          >
            <div className={v.expandInner}>
              <p className={v.desc}>{tone.description}</p>
              <div className={v.files}>
                {tone.models.map((m) => (
                  <FileRow key={m.id} tone={tone} model={m} />
                ))}
              </div>
              <div className={v.tags}>
                {tone.makes.map((m) => (
                  <span key={m} className={v.make}>
                    {m}
                  </span>
                ))}
                {tone.tags.map((t) => (
                  <span key={t} className={v.tag}>
                    #{t}
                  </span>
                ))}
              </div>
            </div>
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  );
}

/* ───────────────────────────────── shell ──────────────────────────────── */

type Tab = "models" | "irs" | "favorites";

const TABS: { value: Tab; label: string }[] = [
  { value: "models", label: "Models" },
  { value: "irs", label: "IRs" },
  { value: "favorites", label: "Favorites" },
];

const SORT_OPTIONS: PillOption[] = (Object.keys(SORT_LABELS) as ProtoSort[]).map(
  (k) => ({ value: k, label: SORT_LABELS[k] }),
);

const GEAR_OPTIONS: PillOption[] = [
  { value: null, label: "Any gear" },
  { value: "amp", label: GEAR_LABELS.amp },
  { value: "amp-cab", label: GEAR_LABELS["amp-cab"] },
  { value: "pedal", label: GEAR_LABELS.pedal },
];

const SIZE_OPTIONS: PillOption[] = [
  { value: null, label: "Any size" },
  { value: "standard", label: "Standard" },
  { value: "lite", label: "Lite" },
  { value: "feather", label: "Feather" },
  { value: "nano", label: "Nano" },
];

const SKELETON_ROWS = [0, 1, 2, 3, 4, 5, 6];

export function VariantB() {
  const reduced = useReducedMotion() ?? false;
  const favorites = useProtoStore((st) => st.favorites);
  const toggleFavorite = useProtoStore((st) => st.toggleFavorite);
  /* Snapshot so the optimistic heart count only moves for *this* session's
     changes rather than double-counting the seeded favorites. */
  const [seeded] = useState(() => new Set(useProtoStore.getState().favorites));

  const [tab, setTab] = useState<Tab>("models");
  const [sort, setSort] = useState<ProtoSort>("trending");
  const [query, setQuery] = useState("");
  const [gear, setGear] = useState<string | null>(null);
  const [size, setSize] = useState<string | null>(null);

  /* Results are keyed by the query that produced them, so "loading" is derived
     (`data.key !== key`) rather than a second state flipped inside the effect. */
  const [data, setData] = useState<{ key: string; rows: ProtoTone[] }>({
    key: "",
    rows: [],
  });
  const [active, setActive] = useState(0);
  const [expandedId, setExpandedId] = useState<number | null>(null);
  const [openMenu, setOpenMenu] = useState<string | null>(null);

  const listRef = useRef<HTMLDivElement>(null);
  const close = useCallback(() => setVariantParam(null), []);

  /* Favorites is a mixed bag (models + IRs), so that tab re-queries both kinds
     and concatenates — it is a view over the catalog, not a third catalog.
     Only that tab depends on the favorites set, so hearting a row elsewhere
     never triggers a re-fetch flash. */
  const favSig =
    tab === "favorites" ? [...favorites].sort((a, b) => a - b).join(",") : "";
  const key = JSON.stringify([tab, sort, query, gear, size, favSig]);
  const loading = data.key !== key;
  const rows = data.rows;

  useEffect(() => {
    let alive = true;
    const favs = useProtoStore.getState().favorites;
    const base = { sort, query, favoritesOnly: tab === "favorites" };
    const request =
      tab === "favorites"
        ? Promise.all([
            fetchCatalog({ ...base, kind: "models", gear: null, size: null }, favs),
            fetchCatalog({ ...base, kind: "irs", gear: null, size: null }, favs),
          ]).then(([a, b]) => [...a, ...b])
        : fetchCatalog({ ...base, kind: tab, gear, size }, favs);
    void request.then((next) => {
      if (!alive) return;
      setData({ key, rows: next });
      setActive(0);
      setExpandedId(null);
    });
    return () => {
      alive = false;
    };
  }, [key, tab, sort, query, gear, size]);

  /* Esc closes the palette from anywhere, including the search field. */
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== "Escape") return;
      if (openMenu) setOpenMenu(null);
      else close();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [close, openMenu]);

  useEffect(() => {
    listRef.current
      ?.querySelector<HTMLElement>(`[data-row="${active}"]`)
      ?.scrollIntoView({ block: "nearest" });
  }, [active, expandedId]);

  const onKeyDown = (e: React.KeyboardEvent) => {
    if (rows.length === 0) return;
    if (e.key === "ArrowDown") {
      e.preventDefault();
      setActive((i) => Math.min(rows.length - 1, i + 1));
    } else if (e.key === "ArrowUp") {
      e.preventDefault();
      setActive((i) => Math.max(0, i - 1));
    } else if (e.key === "Enter") {
      e.preventDefault();
      const row = rows[active];
      if (row) setExpandedId((id) => (id === row.id ? null : row.id));
    }
  };

  const filtersOn = tab === "models";
  const heading = useMemo(() => {
    if (loading) return "Searching…";
    const n = rows.length;
    return `${n} ${n === 1 ? "result" : "results"}`;
  }, [loading, rows.length]);

  return (
    <div className={v.overlay} onKeyDown={onKeyDown}>
      <motion.div
        className={v.scrim}
        initial={{ opacity: 0 }}
        animate={{ opacity: 1 }}
        transition={
          reduced ? noMotion : { duration: duration.base, ease: ease.out }
        }
        onPointerDown={close}
      />

      <motion.div
        role="dialog"
        aria-modal="true"
        aria-label="TONE3000 browser"
        className={v.palette}
        initial={{ opacity: 0, scale: 0.955, y: 12 }}
        animate={{ opacity: 1, scale: 1, y: 0 }}
        transition={reduced ? noMotion : spring.ui}
      >
        {/* ── search ── */}
        <div className={v.searchRow}>
          <SearchGlyph width={16} height={16} className={v.searchIcon} />
          <input
            className={v.searchInput}
            value={query}
            autoFocus
            spellCheck={false}
            placeholder="Search tones, creators, makes…   optional — the filters below browse without a keyboard"
            onChange={(e) => setQuery(e.target.value)}
          />
          {loading && <Spinner size={14} className={v.searchSpinner} />}
          {query !== "" && (
            <button type="button" className={v.clear} onClick={() => setQuery("")}>
              Clear
            </button>
          )}
          <button
            type="button"
            className={v.escBtn}
            aria-label="Close browser"
            onClick={close}
          >
            <Kbd>esc</Kbd>
          </button>
        </div>

        {/* ── tabs + filter pills ── */}
        <div className={v.controlRow}>
          <div className={v.tabs} role="tablist">
            {TABS.map((t) => {
              const on = t.value === tab;
              return (
                <button
                  key={t.value}
                  type="button"
                  role="tab"
                  aria-selected={on}
                  className={cx(v.tab, on && v.tabOn)}
                  onClick={() => setTab(t.value)}
                >
                  {on && (
                    <motion.span
                      layoutId="variantb-tab-pill"
                      className={v.tabPill}
                      transition={reduced ? noMotion : spring.ui}
                    />
                  )}
                  <span className={v.tabLabel}>
                    {t.label}
                    {t.value === "favorites" && favorites.size > 0 && (
                      <span className={v.tabCount}>{favorites.size}</span>
                    )}
                  </span>
                </button>
              );
            })}
          </div>

          <div className={v.pills}>
            <PillMenu
              label="Sort"
              value={sort}
              options={SORT_OPTIONS}
              open={openMenu === "sort"}
              onOpen={() => setOpenMenu((m) => (m === "sort" ? null : "sort"))}
              onChange={(next) => {
                setSort((next ?? "trending") as ProtoSort);
                setOpenMenu(null);
              }}
            />
            {filtersOn && (
              <>
                <PillMenu
                  label="Gear"
                  value={gear}
                  options={GEAR_OPTIONS}
                  open={openMenu === "gear"}
                  onOpen={() => setOpenMenu((m) => (m === "gear" ? null : "gear"))}
                  onChange={(next) => {
                    setGear(next);
                    setOpenMenu(null);
                  }}
                />
                <PillMenu
                  label="Size"
                  value={size}
                  options={SIZE_OPTIONS}
                  open={openMenu === "size"}
                  onOpen={() => setOpenMenu((m) => (m === "size" ? null : "size"))}
                  onChange={(next) => {
                    setSize(next);
                    setOpenMenu(null);
                  }}
                />
              </>
            )}
          </div>
        </div>

        {/* ── results ── */}
        <div className={v.list} ref={listRef}>
          {loading ? (
            <div className={v.skeletons}>
              {SKELETON_ROWS.map((i) => (
                <div key={i} className={v.skelRow}>
                  <Skeleton width={40} height={40} radius="var(--radius-sm)" />
                  <div className={v.skelText}>
                    <Skeleton width={`${46 - i * 3}%`} height={11} />
                    <Skeleton width={`${28 - i * 2}%`} height={9} />
                  </div>
                  <Skeleton width={54} height={11} />
                </div>
              ))}
            </div>
          ) : rows.length === 0 ? (
            <EmptyState
              className={v.empty}
              message={
                tab === "favorites"
                  ? "No favorites yet"
                  : "Nothing matches those filters"
              }
              hint={
                tab === "favorites"
                  ? "Tap the heart on any row to keep it here."
                  : "Reset the gear or size pill to widen the search."
              }
            />
          ) : (
            rows.map((tone, i) => {
              const fav = favorites.has(tone.id);
              const delta = (fav ? 1 : 0) - (seeded.has(tone.id) ? 1 : 0);
              return (
                <ToneRow
                  key={tone.id}
                  tone={tone}
                  index={i}
                  active={i === active}
                  expanded={expandedId === tone.id}
                  favorited={fav}
                  favCount={tone.favoritesCount + delta}
                  reduced={reduced}
                  onActivate={() => setActive(i)}
                  onToggle={() => {
                    setActive(i);
                    setExpandedId((id) => (id === tone.id ? null : tone.id));
                  }}
                  onFavorite={() => toggleFavorite(tone.id)}
                />
              );
            })
          )}
        </div>

        {/* ── footer ── */}
        <div className={v.footer}>
          <span className={v.footCount}>{heading}</span>
          <div className={v.footRight}>
            <span className={v.hints}>
              <Kbd>↑</Kbd>
              <Kbd>↓</Kbd>
              <span>move</span>
              <Kbd>↵</Kbd>
              <span>expand</span>
            </span>
            <span className={v.poweredBy}>
              <span className={v.t3kMark}>T3K</span>
              Powered by TONE3000
            </span>
          </div>
        </div>

        {openMenu && (
          <div className={v.menuScrim} onPointerDown={() => setOpenMenu(null)} />
        )}
      </motion.div>
    </div>
  );
}
