/**
 * Level 1 — browse. Kind, shelf, sort and gear chips drive the request
 * directly; the search field is sugar on top. Hosts such as Logic give the
 * WebView no keyboard at all, so the field is a button that opens a native
 * text prompt (`t3kPromptSearch`), which does get one. Where the page does
 * receive keys (the standalone app, a browser), typing anywhere in the list
 * turns the field into a real input.
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
  cx,
  useMenu,
  type MenuEntry,
  type SegmentedOption,
} from "../../components";
import { bridge, type AmpToken, type CabToken, type T3kBrowseKind } from "../../bridge";
import {
  AMP_TOKENS,
  CAB_TOKENS,
  isAmpToken,
  isCabToken,
  selectT3kTarget,
  t3kTargetLabel,
  useStore,
} from "../../store";
import { SearchIcon } from "./parts";
import { RowSkeleton, ToneRow } from "./ToneRow";

/** Rows appended by "load more" restart the entrance order instead of all
 *  waiting for the cap. */
const PAGE_STAGGER_SPAN = 20;
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
  { value: "models", label: "Captures" },
  { value: "irs", label: "IRs" },
];

/* Offered only while both amps (or both cabs) are placed, so they need their
   numbers. */
const TARGET_OPTIONS: SegmentedOption<AmpToken>[] = [
  { value: "amp", label: "Amp 1" },
  { value: "amp2", label: "Amp 2" },
];
const CAB_TARGET_OPTIONS: SegmentedOption<CabToken>[] = [
  { value: "cab", label: "Cab 1" },
  { value: "cab2", label: "Cab 2" },
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
  const setCabTarget = useStore((st) => st.setT3kCabTarget);
  const setScroll = useStore((st) => st.setT3kScroll);
  const target = useStore(selectT3kTarget);
  const bothAmps = useStore((st) => AMP_TOKENS.every((t) => st.chainOrder.includes(t)));
  const bothCabs = useStore((st) => CAB_TOKENS.every((t) => st.chainOrder.includes(t)));
  const targetLabel = useStore((st) => t3kTargetLabel(target, st.chainOrder));

  const { kind, shelf, query, gear, calibrated } = browser;
  const sort = effectiveSort(browser.sort, query);
  const searching = query.trim() !== "";
  const profile = shelf !== "all";

  /* ── search ── */
  const [search, setSearch] = useState(query);
  const [syncedQuery, setSyncedQuery] = useState(query);
  /* The keyboard path: true while the field is a live <input>. */
  const [editing, setEditing] = useState(false);
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
    if (!editing || trimmed === query.trim() || tooShort) return;
    const id = window.setTimeout(() => patch({ query: trimmed }), SEARCH_DEBOUNCE_MS);
    return () => window.clearTimeout(id);
  }, [editing, trimmed, tooShort, query, patch]);

  const submit = (text: string): void => patch({ query: text.trim() });
  const clearSearch = (): void => {
    setSearch("");
    setEditing(false);
    submit("");
  };

  const promptSearch = async (): Promise<void> => {
    const res = await bridge.t3kPromptSearch(query);
    if (res.text === null) return;
    setSearch(res.text);
    submit(res.text);
  };

  /* A printable key while the list is in front starts typing a search — only
     ever reached where the host delivers keys at all. */
  useEffect(() => {
    if (editing) return;
    const onKey = (e: KeyboardEvent): void => {
      if (e.key.length !== 1 || e.metaKey || e.ctrlKey || e.altKey) return;
      if (e.key === " " && search === "") return;
      const active = document.activeElement;
      if (active instanceof HTMLInputElement || active instanceof HTMLTextAreaElement) return;
      if (document.querySelector('[role="menu"], [role="dialog"]')) return;
      if (useStore.getState().browser.detail) return;
      e.preventDefault();
      setSearch(query === "" ? e.key : query + e.key);
      setEditing(true);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [editing, query, search]);

  /* ── sort menu (the Calibrated filter lives in it too) ── */
  const {
    ref: sortRef,
    anchor: sortAnchor,
    open: sortOpen,
    toggle: toggleSort,
    close: closeSort,
  } = useMenu<HTMLSpanElement>();

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
    setEditing(false);
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

  const placeholder = profile ? "Search this list" : "Search TONE3000";

  return (
    <>
      <header className={s.head}>
        <span className={s.title}>TONE3000</span>
        <span className={s.headSpacer} />
        {kind === "models" && bothAmps && isAmpToken(target) ? (
          <Segmented
            options={TARGET_OPTIONS}
            value={target}
            onChange={(value) => setAmpTarget(value)}
            aria-label="Load captures into"
          />
        ) : kind === "irs" && bothCabs && isCabToken(target) ? (
          <Segmented
            options={CAB_TARGET_OPTIONS}
            value={target}
            onChange={(value) => setCabTarget(value)}
            aria-label="Load IRs into"
          />
        ) : (
          <span className={s.targetText}>Into {targetLabel}</span>
        )}
        <IconButton aria-label="Close browser" onClick={onClose} size="sm">
          <CloseIcon />
        </IconButton>
      </header>

      <div className={s.filters}>
        <div className={s.filterRow}>
          <Segmented
            options={KIND_OPTIONS}
            value={kind}
            onChange={(next) => patch({ kind: next })}
            aria-label="Catalog kind"
          />
          <span ref={sortRef} className={s.sortAnchor}>
            <Button
              size="sm"
              onClick={toggleSort}
              aria-haspopup="menu"
              aria-expanded={sortOpen}
              aria-label={`Sort: ${sortLabel(sort)}${filterOn ? ", calibrated only" : ""}`}
            >
              {sortLabel(sort)}
              {filterOn && " · Calibrated"}
              <ChevronDownIcon size={14} />
            </Button>
          </span>
          <Menu
            anchor={sortAnchor}
            open={sortOpen}
            onClose={closeSort}
            entries={sortEntries}
            align="end"
            minWidth={184}
          />
        </div>

        <div className={cx(s.field, (editing || searching) && s.fieldActive)}>
          <span className={s.fieldIcon}>
            <SearchIcon size={14} />
          </span>
          {editing ? (
            <input
              className={s.fieldInput}
              value={search}
              placeholder={placeholder}
              aria-label={placeholder}
              autoFocus
              onChange={(e) => setSearch(e.target.value)}
              onBlur={() => {
                submit(search);
                setEditing(false);
              }}
              onKeyDown={(e) => {
                if (e.key === "Enter") submit(search);
                if (e.key === "Escape") {
                  e.stopPropagation();
                  clearSearch();
                }
              }}
              enterKeyHint="search"
              spellCheck={false}
            />
          ) : (
            <button
              type="button"
              className={s.fieldButton}
              aria-label="Type a search in a dialog"
              onClick={() => void promptSearch()}
            >
              {searching ? (
                <span className={s.fieldValue}>{query}</span>
              ) : (
                <span className={s.fieldPlaceholder}>{placeholder}</span>
              )}
            </button>
          )}
          {(searching || (editing && search !== "")) && (
            <IconButton
              className={s.fieldClear}
              aria-label="Clear search"
              size="sm"
              onPointerDown={(e) => e.preventDefault()}
              onClick={clearSearch}
            >
              <CloseIcon size={14} />
            </IconButton>
          )}
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
          {filtered && (
            <span className={s.reset}>
              <Button size="sm" variant="ghost" onClick={resetFilters}>
                Reset
              </Button>
            </span>
          )}
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
            message={list.error}
            action={
              <Button size="sm" onClick={() => void fetchPage(1)}>
                Retry
              </Button>
            }
          />
        ) : list.tones.length === 0 ? (
          <EmptyState
            className={s.empty}
            message="No tones"
            hint={
              shelf !== "all" && !searching && gear === null ? SHELF_EMPTY[shelf] : undefined
            }
            action={
              filtered ? (
                <Button size="sm" onClick={resetFilters}>
                  Reset filters
                </Button>
              ) : undefined
            }
          />
        ) : (
          <ul className={s.list}>
            <AnimatePresence initial={false}>
              {list.tones.map((tone, index) => (
                <ToneRow
                  key={tone.id}
                  tone={tone}
                  index={index % PAGE_STAGGER_SPAN}
                  reduced={reduced}
                  onOpen={setDetail}
                  onToggleFavorite={toggleFavorite}
                />
              ))}
            </AnimatePresence>

            {list.error ? (
              <li className={s.moreRow}>
                <span className={s.moreText}>{list.error}</span>
                <Button size="sm" variant="ghost" onClick={() => void fetchPage(nextPage)}>
                  Retry
                </Button>
              </li>
            ) : list.loadingMore ? (
              <li className={s.moreRow} aria-busy="true">
                <span className={s.moreText}>
                  <Spinner size={12} /> Loading more
                </span>
              </li>
            ) : (
              list.hasMore && (
                /* The observer normally fires long before this is visible; the
                   button is the fallback (no IntersectionObserver, tiny lists). */
                <li className={s.moreRow} ref={sentinelRef}>
                  <Button size="sm" variant="ghost" block onClick={() => void fetchPage(nextPage)}>
                    Load more
                  </Button>
                </li>
              )
            )}
          </ul>
        )}
      </div>
    </>
  );
}
