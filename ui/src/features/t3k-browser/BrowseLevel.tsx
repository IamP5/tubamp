/**
 * Level 1 — browse. Kind, shelf, sort and gear chips drive the request
 * directly; the search box is sugar on top. Hosts such as Logic give the
 * WebView no keyboard at all, so typing is never the only way in: the keyboard
 * button opens a native text prompt (`t3kPromptSearch`) that does get one.
 *
 * Search is cheap on the API: 300 ms debounce, nothing below 2 characters
 * (empty clears), Enter submits at once, and the store's list key makes a newer
 * request supersede an older one.
 */
import { useEffect, useLayoutEffect, useRef, useState } from "react";
import { AnimatePresence } from "motion/react";
import {
  Button,
  ChevronDownIcon,
  CloseIcon,
  EmptyState,
  IconButton,
  Menu,
  Segmented,
  Spinner,
  Tooltip,
  cx,
  useMenu,
  type MenuEntry,
  type SegmentedOption,
} from "../../components";
import { bridge, type T3kBrowseKind } from "../../bridge";
import { selectT3kTarget, T3K_TARGET_LABELS, useStore } from "../../store";
import { KeyboardIcon, SearchIcon, T3kMark } from "./parts";
import { RowSkeleton, ToneRow } from "./ToneRow";
import {
  effectiveSort,
  GEAR_FILTERS,
  GEAR_LABELS,
  SHELF_EMPTY,
  SHELVES,
  SORT_LABELS,
  SORTS,
} from "./labels";
import type { BrowseList } from "./useBrowse";
import s from "./t3k.module.css";

const KIND_OPTIONS: SegmentedOption<T3kBrowseKind>[] = [
  { value: "models", label: "Models" },
  { value: "irs", label: "IRs" },
];

const TARGET_OPTIONS: SegmentedOption<"amp" | "amp2">[] = [
  { value: "amp", label: "Amp" },
  { value: "amp2", label: "Amp 2" },
];

const SEARCH_DEBOUNCE_MS = 300;
const SEARCH_MIN_CHARS = 2;
/** Start the next page this far before the end of the list scrolls into view. */
const PAGE_AHEAD_PX = 320;

export interface BrowseLevelProps {
  list: BrowseList;
  reduced: boolean;
  onClose(): void;
}

export function BrowseLevel({ list, reduced, onClose }: BrowseLevelProps) {
  const browser = useStore((st) => st.browser);
  const patch = useStore((st) => st.patchT3kBrowser);
  const fetchPage = useStore((st) => st.t3kFetchPage);
  const setDetail = useStore((st) => st.setT3kDetail);
  const toggleFavorite = useStore((st) => st.t3kToggleFavorite);
  const setAmpTarget = useStore((st) => st.setT3kAmpTarget);
  const setScroll = useStore((st) => st.setT3kScroll);
  const target = useStore(selectT3kTarget);
  const amp2InChain = useStore((st) => st.chainOrder.includes("amp2"));

  const { kind, shelf, query, gear, calibrated } = browser;
  const sort = effectiveSort(browser.sort, query);
  const searching = query.trim() !== "";
  const profile = shelf !== "all";

  /* ── search ── */
  const [search, setSearch] = useState(query);
  const [syncedQuery, setSyncedQuery] = useState(query);
  // The store's query moved without us (native prompt, retarget): follow it.
  // Adjusted during render, not in an effect, so no stale frame renders.
  if (syncedQuery !== query) {
    setSyncedQuery(query);
    // Only when it differs in substance: echoing our own debounced patch back
    // would eat the trailing space of a word still being typed.
    if (search.trim() !== query.trim()) setSearch(query);
  }
  const trimmed = search.trim();
  const tooShort = trimmed.length > 0 && trimmed.length < SEARCH_MIN_CHARS;

  useEffect(() => {
    if (trimmed === query.trim() || tooShort) return;
    const id = window.setTimeout(() => patch({ query: trimmed }), SEARCH_DEBOUNCE_MS);
    return () => window.clearTimeout(id);
  }, [trimmed, tooShort, query, patch]);

  const submit = (text: string): void => patch({ query: text.trim() });

  const promptSearch = async (): Promise<void> => {
    const res = await bridge.t3kPromptSearch(search);
    if (res.text === null) return;
    setSearch(res.text);
    submit(res.text);
  };

  /* ── sort + filters menu ── */
  const {
    ref: sortRef,
    anchor: sortAnchor,
    open: sortOpen,
    toggle: toggleSort,
    close: closeSort,
  } = useMenu();

  const canCalibrate = kind === "models" && !profile;
  /* A profile list has no trending or relevance signal: it keeps the
     endpoint's own order, most recent first — name it that. */
  const sortLabel = (option: typeof sort): string =>
    profile && (option === "trending" || option === "best-match")
      ? "Most recent"
      : SORT_LABELS[option];
  const sortEntries: MenuEntry[] = SORTS.filter(
    (option) => option !== "best-match" || (searching && !profile),
  ).map((option) => ({
    id: option,
    label: sortLabel(option),
    hint:
      option === "best-match" && browser.sort === null
        ? "default"
        : option === "trending" && browser.sort === null && (!searching || profile)
          ? "default"
          : undefined,
    selected: option === sort || (profile && option === "trending" && sort === "best-match"),
    onSelect: () => patch({ sort: option }),
  }));
  if (canCalibrate) {
    sortEntries.push(
      { kind: "separator", id: "sep" },
      {
        id: "calibrated",
        label: "Calibrated only",
        hint: "filter",
        selected: calibrated,
        onSelect: () => patch({ calibrated: !calibrated }),
      },
    );
  }
  const filterOn = canCalibrate && calibrated;

  const filtered = gear !== null || filterOn || shelf !== "all" || searching;
  const resetFilters = (): void => {
    setSearch("");
    patch({ gear: null, shelf: "all", calibrated: false, query: "" });
  };

  /* ── scroll memory + auto paging ── */
  const scrollRef = useRef<HTMLDivElement>(null);
  const sentinelRef = useRef<HTMLLIElement>(null);
  const savedTop = useStore.getState().t3kScroll[list.key] ?? 0;
  const hasRows = list.tones.length > 0;

  useLayoutEffect(() => {
    const el = scrollRef.current;
    if (el && hasRows) el.scrollTop = savedTop;
    // Restore once per list (and when its first rows arrive), not per page.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [list.key, hasRows]);

  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return;
    let timer = 0;
    // Tracked as we go: by the time an unmount cleanup runs the node is
    // detached and reads scrollTop as 0.
    let top = el.scrollTop;
    const key = list.key;
    const onScroll = (): void => {
      top = el.scrollTop;
      window.clearTimeout(timer);
      timer = window.setTimeout(() => setScroll(key, top), 120);
    };
    el.addEventListener("scroll", onScroll, { passive: true });
    return () => {
      window.clearTimeout(timer);
      el.removeEventListener("scroll", onScroll);
      setScroll(key, top);
    };
  }, [list.key, setScroll]);

  const canPage = list.hasMore && !list.loadingMore && list.error === null;
  const nextPage = list.page + 1;
  useEffect(() => {
    const root = scrollRef.current;
    const sentinel = sentinelRef.current;
    if (!root || !sentinel || !canPage || typeof IntersectionObserver === "undefined")
      return;
    // Re-created whenever a page lands, so a sentinel that is still in view
    // after a short page reports again (an observer only fires on change).
    const observer = new IntersectionObserver(
      (entries) => {
        if (entries.some((e) => e.isIntersecting)) void fetchPage(nextPage);
      },
      { root, rootMargin: `0px 0px ${PAGE_AHEAD_PX}px 0px` },
    );
    observer.observe(sentinel);
    return () => observer.disconnect();
  }, [canPage, nextPage, list.tones.length, fetchPage]);

  /* ── copy ── */
  const noun = kind === "irs" ? "IRs" : "models";
  const countText = list.loading
    ? "Loading…"
    : list.error && list.tones.length === 0
      ? "Not loaded"
      : `${list.total} ${list.total === 1 ? noun.slice(0, -1) : noun}`;
  const sortText = tooShort
    ? "keep typing…"
    : profile && (sort === "best-match" || sort === "trending")
      ? "most recent first"
      : browser.sort === null && searching
        ? "best match · search default"
        : SORT_LABELS[sort].toLowerCase();

  return (
    <>
      <header className={s.head}>
        <T3kMark compact />
        <span className={s.headSpacer} />
        <span className={s.targetLabel}>Loading into</span>
        {kind === "models" && amp2InChain ? (
          <Segmented
            className={s.targetSwitch}
            options={TARGET_OPTIONS}
            value={target === "amp2" ? "amp2" : "amp"}
            onChange={(value) => setAmpTarget(value)}
            aria-label="Load captures into"
          />
        ) : (
          <span className={s.targetPill}>{T3K_TARGET_LABELS[target]}</span>
        )}
        <IconButton aria-label="Close browser" onClick={onClose} size="sm">
          <CloseIcon size={14} />
        </IconButton>
      </header>

      <div className={s.controls}>
        <div className={s.controlRow}>
          <Segmented
            options={KIND_OPTIONS}
            value={kind}
            onChange={(next) => patch({ kind: next })}
            aria-label="Catalog kind"
          />
          <button
            ref={sortRef}
            type="button"
            className={cx(s.sortButton, sortOpen && s.sortButtonOpen)}
            onClick={toggleSort}
            aria-haspopup="menu"
            aria-expanded={sortOpen}
            aria-label={`Sort: ${sortLabel(sort)}${filterOn ? ", calibrated only" : ""}`}
          >
            {filterOn && <span className={s.filterDot} aria-hidden="true" />}
            <span className={s.sortLabel}>{sortLabel(sort)}</span>
            <ChevronDownIcon size={12} />
          </button>
          <Menu
            anchor={sortAnchor}
            open={sortOpen}
            onClose={closeSort}
            entries={sortEntries}
            align="end"
            minWidth={184}
          />
        </div>

        <div className={s.searchLine}>
          <label className={s.searchRow}>
            <SearchIcon />
            <input
              className={s.searchInput}
              value={search}
              placeholder={profile ? "Search this list" : "Search TONE3000"}
              onChange={(e) => setSearch(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === "Enter") submit(search);
                if (e.key === "Escape" && search !== "") {
                  e.stopPropagation();
                  setSearch("");
                  submit("");
                }
              }}
              enterKeyHint="search"
              spellCheck={false}
            />
            {search !== "" && (
              <button
                type="button"
                className={s.searchClear}
                aria-label="Clear search"
                onClick={() => {
                  setSearch("");
                  submit("");
                }}
              >
                <CloseIcon size={11} />
              </button>
            )}
          </label>
          <Tooltip label="Type in a dialog (for hosts without keyboard)">
            <button
              type="button"
              className={s.keyboardButton}
              aria-label="Type a search in a dialog"
              onClick={() => void promptSearch()}
            >
              <KeyboardIcon />
            </button>
          </Tooltip>
        </div>

        <div className={s.shelves} role="tablist" aria-label="Shelf">
          {SHELVES.map((option) => (
            <button
              key={option.value}
              type="button"
              role="tab"
              aria-selected={shelf === option.value}
              className={cx(s.shelf, shelf === option.value && s.shelfOn)}
              onClick={() => patch({ shelf: option.value })}
            >
              {option.label}
            </button>
          ))}
        </div>

        <div className={s.chips}>
          {GEAR_FILTERS[kind].map((value) => (
            <button
              key={value}
              type="button"
              className={cx(s.chip, gear === value && s.chipOn)}
              aria-pressed={gear === value}
              onClick={() => patch({ gear: gear === value ? null : value })}
            >
              {GEAR_LABELS[value]}
            </button>
          ))}
        </div>

        <div className={s.countLine}>
          <span className={s.countText}>
            {countText} · {sortText}
            {filterOn && " · calibrated"}
          </span>
          <span className={s.countRight}>
            {(list.loading || list.loadingMore) && <Spinner size={11} />}
            {filtered && (
              <button type="button" className={s.clearButton} onClick={resetFilters}>
                Reset
              </button>
            )}
          </span>
        </div>
      </div>

      <div className={s.scroll} ref={scrollRef}>
        {list.loading ? (
          <ul className={s.list} aria-busy="true">
            {[0, 1, 2, 3, 4, 5].map((i) => (
              <RowSkeleton key={i} />
            ))}
          </ul>
        ) : list.error && list.tones.length === 0 ? (
          <EmptyState
            className={s.empty}
            message="Couldn’t load tones from TONE3000"
            hint={list.error}
            action={
              <Button size="sm" onClick={() => void fetchPage(1)}>
                Try again
              </Button>
            }
          />
        ) : list.tones.length === 0 ? (
          <EmptyState
            className={s.empty}
            message={
              shelf !== "all" && !searching && gear === null
                ? "Nothing here yet"
                : "No tones match"
            }
            hint={
              shelf !== "all" && !searching && gear === null
                ? SHELF_EMPTY[shelf]
                : searching
                  ? "Try a different search or fewer filters."
                  : "Try fewer filters."
            }
            action={
              filtered ? (
                <Button size="sm" onClick={resetFilters}>
                  Show all {noun}
                </Button>
              ) : undefined
            }
          />
        ) : (
          <ul className={s.list}>
            <AnimatePresence initial={false}>
              {list.tones.map((tone, i) => (
                <ToneRow
                  key={tone.id}
                  tone={tone}
                  index={i % 25}
                  reduced={reduced}
                  onOpen={setDetail}
                  onToggleFavorite={toggleFavorite}
                />
              ))}
            </AnimatePresence>

            {list.error ? (
              <li className={s.moreRow}>
                <span className={s.moreError}>{list.error}</span>
                <Button
                  size="sm"
                  variant="ghost"
                  onClick={() => void fetchPage(nextPage)}
                >
                  Retry
                </Button>
              </li>
            ) : list.loadingMore ? (
              <li className={s.moreRow} aria-busy="true">
                <span className={s.moreBusy}>
                  <Spinner size={12} /> Loading more…
                </span>
              </li>
            ) : list.hasMore ? (
              /* The observer normally fires long before this is visible; the
                 button is the fallback (no IntersectionObserver, tiny lists). */
              <li className={s.moreRow} ref={sentinelRef}>
                <Button
                  size="sm"
                  variant="ghost"
                  block
                  onClick={() => void fetchPage(nextPage)}
                >
                  Load more
                </Button>
              </li>
            ) : (
              list.tones.length > 8 && (
                <li className={s.endRow}>
                  End of list · {list.total} {noun}
                </li>
              )
            )}
          </ul>
        )}
      </div>
    </>
  );
}
