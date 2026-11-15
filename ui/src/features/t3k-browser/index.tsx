/**
 * TONE3000 browser — a 400px (--t3k-w) drawer on the right edge, header bottom
 * to window bottom, no backdrop (docs/REACT-UI.md §TONE3000 browser). The board
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
import { BLOCK_INFO } from "../../theme/blocks";
import { ease, noMotion } from "../../theme/motion";
import {
  AMP_SLOTS,
  CAB_SLOTS,
  decorateTone,
  isCabToken,
  loadedPathOf,
  selectT3kTarget,
  useStore,
} from "../../store";
import { BrowseLevel } from "./BrowseLevel";
import { ToneDetail } from "./ToneDetail";
import { SignInSplash } from "./SignInSplash";
import { useBrowse } from "./useBrowse";
import s from "./t3k.module.css";

/* DESIGN.md §Motion: overlays 240 ms, a push inside one 180 ms, one easing. */
const SLIDE = { duration: 0.24, ease: ease.standard };
const PUSH = { duration: 0.18, ease: ease.standard };

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
    // The tone's title when the file came from TONE3000, never its file slug.
    const file = isCabToken(target) ? CAB_SLOTS[target].ir(st) : AMP_SLOTS[target].model(st);
    return file ? file.tone?.title || file.name : null;
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
      data-family={BLOCK_INFO[target].family}
      initial={reduced ? { opacity: 0 } : { x: "100%" }}
      animate={reduced ? { opacity: 1 } : { x: 0 }}
      exit={reduced ? { opacity: 0 } : { x: "100%" }}
      transition={reduced ? noMotion : SLIDE}
      aria-label="TONE3000 browser"
    >
      <div className={s.levels}>
        {authenticated ? (
          <motion.div
            className={s.level}
            initial={false}
            animate={{ opacity: showDetail ? 0 : 1 }}
            transition={reduced ? noMotion : PUSH}
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
              initial={reduced ? { opacity: 0 } : { x: 24, opacity: 0 }}
              animate={reduced ? { opacity: 1 } : { x: 0, opacity: 1 }}
              exit={reduced ? { opacity: 0 } : { x: 24, opacity: 0 }}
              transition={reduced ? noMotion : PUSH}
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

      <footer className={s.foot}>
        <span className={s.footLoaded} title={loadedName ?? undefined}>
          {loadedName ?? (isCabToken(target) ? "No IR loaded" : "No capture loaded")}
        </span>
        <span className={s.footMark}>Powered by TONE3000</span>
      </footer>
    </motion.aside>
  );
}
