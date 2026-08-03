/**
 * One catalog list, paged. `t3kBrowse` answers 25 rows at a time, so the hook
 * appends pages of the same list and replaces everything when the list itself
 * changes (kind / shelf / sort / query / gear).
 *
 * Loading is derived, never stored: the result carries the list key it answered,
 * so a response that lands after the filters moved on can only be ignored, not
 * rendered. Favorites are optimistic overrides layered on top of the server rows
 * — they must not re-trigger a fetch, and they must survive a page append.
 */
import { useCallback, useEffect, useMemo, useState } from "react";
import { bridge, type T3kBrowseRequest, type T3kTone } from "../../bridge";
import { useStore } from "../../store";

interface PageState {
  key: string;
  /** Highest page answered for `key`; 0 before the first response. */
  page: number;
  totalPages: number;
  total: number;
  tones: T3kTone[];
  error: string | null;
}

const EMPTY: PageState = {
  key: "",
  page: 0,
  totalPages: 1,
  total: 0,
  tones: [],
  error: null,
};

/** The request minus its page — two requests sharing this describe one list. */
function listKeyOf(request: T3kBrowseRequest): string {
  return JSON.stringify([
    request.kind,
    request.shelf,
    request.sort,
    request.query,
    request.gear,
  ]);
}

export interface BrowseList {
  tones: T3kTone[];
  /** First page of a new list in flight — show skeletons. */
  loading: boolean;
  /** A further page in flight — keep the rows, spin the "Load more" row. */
  loadingMore: boolean;
  error: string | null;
  total: number;
  hasMore: boolean;
  /** Applies the optimistic favorite overrides to a tone held outside the list. */
  decorate(tone: T3kTone): T3kTone;
  toggleFavorite(tone: T3kTone): void;
}

export function useBrowse(request: T3kBrowseRequest): BrowseList {
  const key = listKeyOf(request);
  const [result, setResult] = useState<PageState>(EMPTY);
  const [overrides, setOverrides] = useState<Record<number, boolean>>({});
  const toast = useStore((st) => st.toast);

  useEffect(() => {
    let cancelled = false;
    void bridge.t3kBrowse(request).then((res) => {
      if (cancelled) return;
      setResult((prev) => {
        const append = request.page > 1 && prev.key === key;
        if (res.error !== undefined) {
          return {
            ...prev,
            key,
            page: request.page,
            tones: append ? prev.tones : [],
            error: res.error,
          };
        }
        const tones = res.tones ?? [];
        return {
          key,
          page: res.page ?? request.page,
          totalPages: res.totalPages ?? 1,
          total: res.total ?? tones.length,
          tones: append ? [...prev.tones, ...tones] : tones,
          error: null,
        };
      });
    });
    return () => {
      cancelled = true;
    };
  }, [request, key]);

  const decorate = useCallback(
    (tone: T3kTone): T3kTone => {
      const override = overrides[tone.id];
      if (override === undefined || override === tone.favorited) return tone;
      return {
        ...tone,
        favorited: override,
        favoritesCount: Math.max(0, tone.favoritesCount + (override ? 1 : -1)),
      };
    },
    [overrides],
  );

  const toggleFavorite = useCallback(
    (tone: T3kTone) => {
      const next = !decorate(tone).favorited;
      setOverrides((o) => ({ ...o, [tone.id]: next }));
      void bridge.t3kSetFavorite(tone.id, next).then((res) => {
        if (!res.error) return;
        setOverrides((o) => ({ ...o, [tone.id]: !next }));
        toast(res.error, "error", 6000);
      });
    },
    [decorate, toast],
  );

  const tones = useMemo(
    () => (result.key === key ? result.tones.map(decorate) : []),
    [result, key, decorate],
  );

  return {
    tones,
    loading: result.key !== key,
    loadingMore: result.key === key && request.page > result.page,
    error: result.key === key ? result.error : null,
    total: result.key === key ? result.total : 0,
    hasMore: result.key === key && result.page < result.totalPages,
    decorate,
    toggleFavorite,
  };
}
