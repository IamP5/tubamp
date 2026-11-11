/**
 * The list the drawer is showing, read from the store.
 *
 * Pages live in the store (`t3kLists`, keyed by the request minus its page), so
 * closing and reopening the drawer renders the same rows instantly with no
 * request. A list is fetched only when its key has never been answered; a
 * response that lands after the filters moved on is cached under its own key
 * and simply not shown. Favorites are optimistic overrides layered on top —
 * they never re-trigger a fetch and survive a page append.
 */
import { useEffect, useMemo } from "react";
import type { T3kTone } from "../../bridge";
import { decorateTone, t3kListKey, useStore } from "../../store";

export interface BrowseList {
  key: string;
  tones: T3kTone[];
  /** First page of a list in flight — show skeletons. */
  loading: boolean;
  /** A further page in flight — keep the rows, show the busy row. */
  loadingMore: boolean;
  error: string | null;
  total: number;
  page: number;
  hasMore: boolean;
}

export function useBrowse(): BrowseList {
  const browser = useStore((st) => st.browser);
  const key = t3kListKey(browser);
  const list = useStore((st) => st.t3kLists[key]);
  const overrides = useStore((st) => st.t3kFavOverrides);
  const fetchPage = useStore((st) => st.t3kFetchPage);

  /* Never answered and nothing in flight: this list's first page. An errored
     list waits for an explicit retry rather than looping on the API. */
  const authenticated = useStore((st) => st.t3k.authenticated);
  const needsFirstPage = list === undefined && authenticated;
  useEffect(() => {
    if (needsFirstPage) void fetchPage(1);
  }, [key, needsFirstPage, fetchPage]);

  const tones = useMemo(
    () => (list ? list.tones.map((t) => decorateTone(t, overrides)) : []),
    [list, overrides],
  );

  const answered = list !== undefined && list.page > 0;
  return {
    key,
    tones,
    loading: !answered && list?.error == null,
    loadingMore: answered && list.loadingPage !== null && list.loadingPage > 1,
    error: list?.error ?? null,
    total: list?.total ?? 0,
    page: list?.page ?? 0,
    hasMore: answered && list.page < list.totalPages,
  };
}
