/**
 * App shell — docs/REACT-UI.md §UX structure, ui/DESIGN.md §Layout.
 *
 * Fluid: header (44) · board (fills) · editor ("dock", a flush band on the
 * stage's bottom edge while a block is selected) · status bar (36). The bands
 * are fixed and everything spare goes to the board — see theme/tokens.css §App
 * metrics for what grows and why, and for the derivation of the minimum size.
 * The dock overlays the board rather than stealing layout, and both are mounted
 * only once the first `getUiState()` has resolved so nothing renders against
 * empty state.
 *
 * Owned here because they are genuinely app-wide:
 *  - settings-sheet open state (the header holds its two triggers),
 *  - global `contextmenu` suppression (Logic's WebView host has crashed on
 *    native context menus; every menu in this UI is ours),
 *  - the entrance choreography (bands fade in, the editor lands last; all of
 *    it inside 240 ms),
 *  - Logic's "touch to select" parameter-under-mouse reporting,
 *  - the window's own corner grip, which has to live in the page (features/
 *    resize explains why there is nowhere else to put it).
 */
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { motion, useReducedMotion } from "motion/react";
import s from "./App.module.css";
import { bridge, isMockBridge } from "./bridge";
import { useStore } from "./store";
import { Toaster, cx } from "./components";
import { duration, ease } from "./theme/motion";
import { Header } from "./features/header";
import { Board } from "./features/board";
import { FxEmbedStage } from "./features/embed";
import { Panel } from "./features/panel";
import { Footer } from "./features/footer";
import { ResizeGrip, useEmulatedWindow } from "./features/resize";
import { SettingsSheet } from "./features/settings";
import { T3kBrowser } from "./features/t3k-browser";

/** The editor lands last: delay + duration.fast stays under 240 ms in total. */
const PANEL_DELAY = 0.08;

/** The bands only fade: no travel, nothing to watch. */
const BAND_IN = { duration: duration.instant, ease: ease.out };

export function App() {
  const hydrate = useStore((st) => st.hydrate);
  const ready = useStore((st) => st.ready);
  const embedded = useStore((st) => st.fxEmbed.slot !== -1);
  const panelOpen = useStore((st) => st.selected !== null);
  const t3kOpen = useStore((st) => st.browser.open);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const openSettings = useCallback(() => setSettingsOpen(true), []);
  const closeSettings = useCallback(() => setSettingsOpen(false), []);
  const reduced = useReducedMotion() ?? false;

  useEffect(() => {
    void hydrate();
  }, [hydrate]);

  /**
   * No native context menu anywhere in the plugin UI. Installed at the app root
   * (not on the board) so it holds while the board is unmounted — during
   * hydration, or with a modal covering the stage.
   */
  useEffect(() => {
    const suppress = (event: MouseEvent): void => event.preventDefault();
    document.addEventListener("contextmenu", suppress);
    return () => document.removeEventListener("contextmenu", suppress);
  }, []);

  /**
   * Host parameter-under-mouse (Logic's "touch to select"): every control
   * carrying `data-param-index` reports itself as the mouse moves over it.
   */
  const paramIndexUpdater = useMemo(
    () => bridge.paramIndexUpdater("data-param-index"),
    [],
  );

  /* No-op under the real bridge, where the shell already is the window. */
  const rootRef = useRef<HTMLDivElement>(null);
  useEmulatedWindow(rootRef);

  return (
    <div
      ref={rootRef}
      className={cx(s.root, isMockBridge && s.rootMock)}
      data-panel-open={panelOpen || undefined}
      data-t3k-open={t3kOpen || undefined}
      onMouseMove={(e) => paramIndexUpdater.handleMouseMove(e.nativeEvent)}
    >
      <motion.header
        className={s.header}
        initial={reduced ? false : { opacity: 0 }}
        animate={{ opacity: 1 }}
        transition={BAND_IN}
      >
        <Header onOpenSettings={openSettings} />
      </motion.header>

      {/* Canvas chrome fades in first and near-instantly, so no frame is blank. */}
      <motion.main
        className={s.stage}
        initial={reduced ? false : { opacity: 0 }}
        animate={{ opacity: 1 }}
        transition={BAND_IN}
      >
        {/* The board stays mounted under an embed so its pan/zoom survives;
            `inert` because the panel above it is opaque but not a focus trap. */}
        <div className={s.board} inert={embedded}>
          {ready && <Board />}
        </div>
        {ready && <FxEmbedStage />}
        <div className={s.dock}>
          {ready && (
            <motion.div
              className={s.dockInner}
              initial={reduced ? false : { opacity: 0, y: 8 }}
              animate={{ opacity: 1, y: 0 }}
              transition={{
                duration: duration.fast,
                ease: ease.out,
                delay: reduced ? 0 : PANEL_DELAY,
              }}
            >
              <Panel />
            </motion.div>
          )}
        </div>
      </motion.main>

      <motion.footer
        className={s.footer}
        initial={reduced ? false : { opacity: 0 }}
        animate={{ opacity: 1 }}
        transition={BAND_IN}
      >
        {ready && <Footer />}
      </motion.footer>

      {/* Docked over the stage's right edge, no backdrop: the board keeps
          working while the catalog is open. */}
      <T3kBrowser />

      <SettingsSheet open={settingsOpen} onClose={closeSettings} />
      <Toaster />
      <ResizeGrip />
    </div>
  );
}
