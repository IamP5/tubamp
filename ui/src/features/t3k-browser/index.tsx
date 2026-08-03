/**
 * TONE3000 browser — a 380px inspector drawer on the right edge, header bottom
 * to app bottom, no backdrop (docs/REACT-UI.md §TONE3000 browser). The board
 * stays visible and clickable while browsing, which is the point of a drawer
 * rather than a modal.
 *
 * Two levels with push navigation: the catalog list, and one tone's detail.
 * Level state is per-opening — the drawer unmounts when closed, so the request
 * always starts from the kind the entry point asked for.
 */
import { useCallback, useEffect, useState } from "react";
import { AnimatePresence, motion, useReducedMotion } from "motion/react";
import { cx, useBlockingOverlay } from "../../components";
import { duration, ease, noMotion, spring } from "../../theme/motion";
import { useStore } from "../../store";
import type { T3kBrowseRequest, T3kTone } from "../../bridge";
import { BrowseLevel } from "./BrowseLevel";
import { ToneDetail } from "./ToneDetail";
import { SignInSplash } from "./SignInSplash";
import { T3kMark } from "./parts";
import { useBrowse } from "./useBrowse";
import s from "./t3k.module.css";

export function T3kBrowser() {
  const open = useStore((st) => st.browser.open);
  return <AnimatePresence>{open && <Drawer />}</AnimatePresence>;
}

/** Two requests are the same list when every field matches — a patch that
 *  changes nothing must not re-run the query. */
function sameRequest(a: T3kBrowseRequest, b: T3kBrowseRequest): boolean {
  return (
    a.kind === b.kind &&
    a.shelf === b.shelf &&
    a.sort === b.sort &&
    a.query === b.query &&
    a.gear === b.gear &&
    a.page === b.page
  );
}

function Drawer() {
  const reduced = useReducedMotion() ?? false;

  /* No backdrop, but it still covers the right third of the board band — which
     is where an embedded plugin's native view sits, and that view would draw
     straight over the drawer. Declaring it makes features/embed stand aside. */
  useBlockingOverlay(true);

  const kind = useStore((st) => st.browser.kind);
  const close = useStore((st) => st.closeT3kBrowser);
  const authenticated = useStore((st) => st.t3k.authenticated);
  const model = useStore((st) => st.model);
  const ir = useStore((st) => st.ir);

  const [request, setRequest] = useState<T3kBrowseRequest>(() => ({
    kind,
    shelf: "all",
    sort: "trending",
    query: "",
    gear: null,
    page: 1,
  }));
  const [detail, setDetail] = useState<T3kTone | null>(null);

  /* Any patch but an explicit page bump starts the list again at page 1. */
  const patch = useCallback((next: Partial<T3kBrowseRequest>) => {
    setRequest((current) => {
      const merged = { ...current, ...next, page: next.page ?? 1 };
      return sameRequest(current, merged) ? current : merged;
    });
  }, []);

  const retry = useCallback(() => setRequest((current) => ({ ...current })), []);

  /* Reopening from the other entry point retargets an already-open drawer.
     Adjusted during render, not in an effect: an effect would render the old
     list once more before switching. */
  const [entryKind, setEntryKind] = useState(kind);
  if (entryKind !== kind) {
    setEntryKind(kind);
    setRequest((current) => ({ ...current, kind, gear: null, page: 1 }));
    setDetail(null);
  }

  useEffect(() => {
    const onKey = (event: KeyboardEvent): void => {
      if (event.key !== "Escape") return;
      if (detail) setDetail(null);
      else close();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [detail, close]);

  const list = useBrowse(request);
  const detailTone = detail ? list.decorate(detail) : null;
  const loaded = request.kind === "irs" ? ir : model;

  return (
    <motion.aside
      className={s.drawer}
      initial={reduced ? { opacity: 0 } : { x: 380 }}
      animate={reduced ? { opacity: 1 } : { x: 0 }}
      exit={reduced ? { opacity: 0 } : { x: 380 }}
      transition={reduced ? noMotion : { ...spring.reflow, mass: 1.1 }}
      aria-label="TONE3000 browser"
    >
      <div className={s.levels}>
        {authenticated ? (
          <motion.div
            className={s.level}
            animate={{ x: detail ? -76 : 0, opacity: detail ? 0.35 : 1 }}
            transition={reduced ? noMotion : spring.ui}
            /* `inert`, not aria-hidden: the pushed-away level may still hold
               focus, which aria-hidden is not allowed to cover. */
            inert={detail !== null}
            style={{ pointerEvents: detail ? "none" : "auto" }}
          >
            <BrowseLevel
              request={request}
              list={list}
              reduced={reduced}
              onPatch={patch}
              onRetry={retry}
              onOpen={setDetail}
              onClose={close}
            />
          </motion.div>
        ) : (
          <div className={s.level}>
            <SignInSplash onClose={close} />
          </div>
        )}

        <AnimatePresence>
          {detailTone && (
            <motion.div
              className={cx(s.level, s.levelTop)}
              initial={reduced ? { opacity: 0 } : { x: "100%" }}
              animate={reduced ? { opacity: 1 } : { x: 0 }}
              exit={reduced ? { opacity: 0 } : { x: "100%" }}
              transition={reduced ? noMotion : { ...spring.ui, restDelta: 0.5 }}
            >
              <ToneDetail
                tone={detailTone}
                onBack={() => setDetail(null)}
                onClose={close}
                onToggleFavorite={list.toggleFavorite}
              />
            </motion.div>
          )}
        </AnimatePresence>
      </div>

      <footer className={s.dockbar}>
        <div className={s.statusLine}>
          <span
            className={cx(s.statusDot, loaded && s.statusDotOn)}
            aria-hidden="true"
          />
          <AnimatePresence mode="wait" initial={false}>
            <motion.span
              key={loaded?.path ?? "none"}
              className={s.statusText}
              initial={reduced ? false : { opacity: 0, y: 4 }}
              animate={{ opacity: 1, y: 0 }}
              exit={reduced ? { opacity: 0 } : { opacity: 0, y: -4 }}
              transition={
                reduced ? noMotion : { duration: duration.fast, ease: ease.out }
              }
            >
              {loaded?.name ??
                (request.kind === "irs" ? "No IR loaded" : "No capture loaded")}
            </motion.span>
          </AnimatePresence>
          {loaded && (
            <span className={s.statusKind}>
              {request.kind === "irs" ? "IR" : "AMP"}
            </span>
          )}
        </div>
        <T3kMark />
      </footer>
    </motion.aside>
  );
}
