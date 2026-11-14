/**
 * TONE3000 browser — a 380px inspector drawer on the right edge, header bottom
 * to app bottom, no backdrop (docs/REACT-UI.md §TONE3000 browser). The board
 * stays visible and clickable while browsing, and the block panel insets its
 * right edge by the drawer's width (App.tsx `data-t3k-open`).
 *
 * Two levels with push navigation: the catalog list, and one tone's detail.
 * Everything that describes where the user is — request, pages, scroll, the
 * open detail tone — lives in the store, so closing and reopening the drawer is
 * instant and lands where they left it. Opening it from another panel only
 * retargets kind/target (store `openT3kBrowser`).
 */
import { useEffect } from "react";
import { AnimatePresence, motion, useReducedMotion } from "motion/react";
import { cx, useBlockingOverlay } from "../../components";
import { duration, ease, noMotion, spring } from "../../theme/motion";
import {
  decorateTone,
  loadedPathOf,
  selectT3kTarget,
  T3K_TARGET_LABELS,
  useStore,
} from "../../store";
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

function Drawer() {
  const reduced = useReducedMotion() ?? false;

  /* No backdrop, but it still covers the right third of the board band — which
     is where an embedded plugin's native view sits, and that view would draw
     straight over the drawer. Declaring it makes features/embed stand aside. */
  useBlockingOverlay(true);

  const close = useStore((st) => st.closeT3kBrowser);
  const authenticated = useStore((st) => st.t3k.authenticated);
  const detail = useStore((st) => st.browser.detail);
  const setDetail = useStore((st) => st.setT3kDetail);
  const overrides = useStore((st) => st.t3kFavOverrides);
  const target = useStore(selectT3kTarget);
  const loadedName = useStore((st) => {
    const path = loadedPathOf(st, target);
    if (path === null) return null;
    if (target === "cab") return st.ir?.name ?? null;
    return (target === "amp2" ? st.modelB?.name : st.model?.name) ?? null;
  });

  useEffect(() => {
    const onKey = (event: KeyboardEvent): void => {
      if (event.key !== "Escape") return;
      // An open menu (sort) takes this Escape for itself.
      if (document.querySelector('[role="menu"]')) return;
      if (useStore.getState().browser.detail) setDetail(null);
      else close();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [setDetail, close]);

  const list = useBrowse();
  const detailTone = detail ? decorateTone(detail, overrides) : null;
  const showDetail = authenticated && detailTone !== null;

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
            initial={false}
            animate={{ x: showDetail ? -76 : 0, opacity: showDetail ? 0.35 : 1 }}
            transition={reduced ? noMotion : spring.ui}
            /* `inert`, not aria-hidden: the pushed-away level may still hold
               focus, which aria-hidden is not allowed to cover. */
            inert={showDetail}
            style={{ pointerEvents: showDetail ? "none" : "auto" }}
          >
            <BrowseLevel list={list} reduced={reduced} onClose={close} />
          </motion.div>
        ) : (
          <div className={s.level}>
            <SignInSplash onClose={close} />
          </div>
        )}

        <AnimatePresence initial={false}>
          {showDetail && (
            <motion.div
              key={detailTone.id}
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
              />
            </motion.div>
          )}
        </AnimatePresence>
      </div>

      <footer className={s.dockbar}>
        <div className={s.statusLine}>
          <span
            className={cx(s.statusDot, loadedName !== null && s.statusDotOn)}
            aria-hidden="true"
          />
          <AnimatePresence mode="wait" initial={false}>
            <motion.span
              key={`${target}:${loadedName ?? "none"}`}
              className={s.statusText}
              initial={reduced ? false : { opacity: 0, y: 4 }}
              animate={{ opacity: 1, y: 0 }}
              exit={reduced ? { opacity: 0 } : { opacity: 0, y: -4 }}
              transition={
                reduced ? noMotion : { duration: duration.fast, ease: ease.out }
              }
            >
              {loadedName ?? (target === "cab" ? "No IR loaded" : "No capture loaded")}
            </motion.span>
          </AnimatePresence>
          <span className={s.statusKind}>{T3K_TARGET_LABELS[target].toUpperCase()}</span>
        </div>
        <T3kMark />
      </footer>
    </motion.aside>
  );
}
