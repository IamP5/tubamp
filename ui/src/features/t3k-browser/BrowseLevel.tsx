/**
 * Level 1 — browse. Kind + sort + chips drive the request directly; the search
 * field is debounced sugar on top (Logic's WebView gets no keyboard, so chips
 * and sort have to carry browsing on their own).
 */
import { useEffect, useState } from "react";
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
import type {
  T3kBrowseKind,
  T3kBrowseRequest,
  T3kTone,
} from "../../bridge";
import { HeartIcon, SearchIcon, T3kMark } from "./parts";
import { RowSkeleton, ToneRow } from "./ToneRow";
import { GEAR_FILTERS, GEAR_LABELS, SORT_LABELS, SORTS } from "./labels";
import type { BrowseList } from "./useBrowse";
import s from "./t3k.module.css";

const KIND_OPTIONS: SegmentedOption<T3kBrowseKind>[] = [
  { value: "models", label: "Models" },
  { value: "irs", label: "IRs" },
];

const SEARCH_DEBOUNCE_MS = 250;

export interface BrowseLevelProps {
  request: T3kBrowseRequest;
  list: BrowseList;
  reduced: boolean;
  onPatch(patch: Partial<T3kBrowseRequest>): void;
  onRetry(): void;
  onOpen(tone: T3kTone): void;
  onClose(): void;
}

export function BrowseLevel({
  request,
  list,
  reduced,
  onPatch,
  onRetry,
  onOpen,
  onClose,
}: BrowseLevelProps) {
  const [search, setSearch] = useState(request.query);
  const {
    ref: sortRef,
    anchor: sortAnchor,
    open: sortOpen,
    toggle: toggleSort,
    close: closeSort,
  } = useMenu();

  /* Typing must not fire a request per keystroke; every other control does. */
  useEffect(() => {
    const id = window.setTimeout(() => onPatch({ query: search }), SEARCH_DEBOUNCE_MS);
    return () => window.clearTimeout(id);
  }, [search, onPatch]);

  const filtered = request.gear !== null || request.shelf === "favorites";
  const clearFilters = (): void => onPatch({ gear: null, shelf: "all" });

  const sortEntries: MenuEntry[] = SORTS.map((sort) => ({
    id: sort,
    label: SORT_LABELS[sort],
    selected: sort === request.sort,
    onSelect: () => onPatch({ sort }),
  }));

  return (
    <>
      <header className={s.head}>
        <T3kMark compact />
        <span className={s.headSpacer} />
        <IconButton aria-label="Close browser" onClick={onClose} size="sm">
          <CloseIcon size={14} />
        </IconButton>
      </header>

      <div className={s.controls}>
        <div className={s.controlRow}>
          <Segmented
            options={KIND_OPTIONS}
            value={request.kind}
            onChange={(kind) => onPatch({ kind, gear: null })}
            aria-label="Catalog kind"
          />
          <button
            ref={sortRef}
            type="button"
            className={cx(s.sortButton, sortOpen && s.sortButtonOpen)}
            onClick={toggleSort}
            aria-haspopup="menu"
            aria-expanded={sortOpen}
          >
            <span className={s.sortLabel}>{SORT_LABELS[request.sort]}</span>
            <ChevronDownIcon size={12} />
          </button>
          <Menu
            anchor={sortAnchor}
            open={sortOpen}
            onClose={closeSort}
            entries={sortEntries}
            align="end"
            minWidth={168}
          />
        </div>

        <label className={s.searchRow}>
          <SearchIcon />
          <input
            className={s.searchInput}
            value={search}
            placeholder="Search — optional"
            onChange={(e) => setSearch(e.target.value)}
          />
          {search !== "" && (
            <button
              type="button"
              className={s.searchClear}
              aria-label="Clear search"
              onClick={() => setSearch("")}
            >
              <CloseIcon size={11} />
            </button>
          )}
        </label>

        <div className={s.chips}>
          {/* Favorites is a server-side shelf (/tones/favorited), not a filter
              over the current page. */}
          <button
            type="button"
            className={cx(s.chip, request.shelf === "favorites" && s.chipOn)}
            onClick={() =>
              onPatch({ shelf: request.shelf === "favorites" ? "all" : "favorites" })
            }
          >
            <HeartIcon filled={request.shelf === "favorites"} size={11} />
            Favorites
          </button>

          {request.kind === "models" ? (
            GEAR_FILTERS.map((gear) => (
              <button
                key={gear}
                type="button"
                className={cx(s.chip, request.gear === gear && s.chipOn)}
                onClick={() =>
                  onPatch({ gear: request.gear === gear ? null : gear })
                }
              >
                {GEAR_LABELS[gear]}
              </button>
            ))
          ) : (
            <span className={s.chipStatic}>{GEAR_LABELS.cab}</span>
          )}
        </div>

        <div className={s.countLine}>
          <span>
            {list.total} {request.kind === "irs" ? "IRs" : "models"} ·{" "}
            {SORT_LABELS[request.sort].toLowerCase()}
          </span>
          <span className={s.countRight}>
            {(list.loading || list.loadingMore) && <Spinner size={11} />}
            {filtered && (
              <button type="button" className={s.clearButton} onClick={clearFilters}>
                Clear
              </button>
            )}
          </span>
        </div>
      </div>

      <div className={s.scroll}>
        {list.loading ? (
          <ul className={s.list}>
            {[0, 1, 2, 3, 4, 5].map((i) => (
              <RowSkeleton key={i} />
            ))}
          </ul>
        ) : list.error && list.tones.length === 0 ? (
          <EmptyState
            className={s.empty}
            message="TONE3000 could not be reached"
            hint={list.error}
            action={
              <Button size="sm" onClick={onRetry}>
                Try again
              </Button>
            }
          />
        ) : list.tones.length === 0 ? (
          <EmptyState
            className={s.empty}
            message="Nothing here yet"
            hint={
              request.shelf === "favorites"
                ? "Heart a tone to pin it to this list."
                : "Loosen a filter to see more captures."
            }
            action={
              filtered ? (
                <Button size="sm" onClick={clearFilters}>
                  Reset filters
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
                  index={i}
                  reduced={reduced}
                  onOpen={onOpen}
                  onToggleFavorite={list.toggleFavorite}
                />
              ))}
            </AnimatePresence>

            {list.error ? (
              <li className={s.moreRow}>
                <span className={s.moreError}>{list.error}</span>
                <Button size="sm" variant="ghost" onClick={onRetry}>
                  Try again
                </Button>
              </li>
            ) : (
              list.hasMore && (
                <li className={s.moreRow}>
                  <Button
                    size="sm"
                    variant="ghost"
                    block
                    disabled={list.loadingMore}
                    onClick={() => onPatch({ page: request.page + 1 })}
                  >
                    {list.loadingMore ? "Loading…" : "Load more"}
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
