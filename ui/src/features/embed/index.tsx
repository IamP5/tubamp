/**
 * The hosted plugin's own editor, drawn inside our window.
 *
 * The thing this file manages is not on the page. The hosted editor is a NATIVE
 * view and a SIBLING of the WebView: it composites ABOVE it, so the rectangle it
 * occupies is a hole punched through everything React can render. Nothing we
 * draw there is visible, and nothing we draw *over* it is either.
 *
 * So the job is three-fold and none of it is painting:
 *
 *  1. Reserve the rectangle and keep C++ told where it is (`fxSetEmbedRect`),
 *     re-measured on any layout change and rate-limited to animation frames.
 *  2. Stand aside whenever something must appear on top. Overlays declare
 *     themselves in components/overlay.tsx and this is the only place that acts
 *     on the declaration, so a future overlay cannot forget to.
 *  3. Take the hosted editor's size seriously. It arrives by event, can arrive
 *     more than once (plenty of AUs only know their real size once the view is
 *     attached), and when it does not fit, the answer is a bigger window rather
 *     than a smaller plugin.
 */
import { useCallback, useEffect, useRef, useSyncExternalStore } from "react";
import {
  Button,
  blockingOverlaysOpen,
  onBlockingOverlaysChange,
  setEmbedHole,
} from "../../components";
import { bridge, isMockBridge, type FxSlotIndex } from "../../bridge";
import { useStore } from "../../store";
import s from "./embed.module.css";

export function FxEmbedStage() {
  const slot = useStore((st) => st.fxEmbed.slot);
  if (slot === -1) return null;
  // Keyed by slot so a switch between two embedded slots restarts the reporting
  // state cleanly instead of carrying the previous rectangle across.
  return <EmbedPanel key={slot} slot={slot} />;
}

interface EmbedPanelProps {
  slot: FxSlotIndex;
}

function EmbedPanel({ slot }: EmbedPanelProps) {
  const name = useStore((st) => st.fxSlots[slot]?.name ?? "");
  const manufacturer = useStore((st) => st.fxSlots[slot]?.manufacturer ?? "");
  /* Two selectors, not one object: a selector that builds an object returns a
     fresh reference on every store write and re-renders on all of them. */
  const wantWidth = useStore((st) => st.fxEmbed.width);
  const wantHeight = useStore((st) => st.fxEmbed.height);
  const unembed = useStore((st) => st.unembedFxEditor);
  const openFxEditor = useStore((st) => st.openFxEditor);

  const hidden = useSyncExternalStore(
    onBlockingOverlaysChange,
    blockingOverlaysOpen,
  );

  const holeRef = useRef<HTMLDivElement | null>(null);
  const frame = useRef(0);
  const sent = useRef("");

  /**
   * Measure the hole and report it.
   *
   * The page cannot scroll (global.css pins the body), so client coordinates and
   * page coordinates are the same thing here.
   *
   * Note what this does NOT do: it never resizes the window. Growing from here
   * meant reacting to our own resize events, which raced the relayout (asking
   * again from a rect measured before the previous grow had landed) and, mid-drag,
   * fought the user's hand. The window floor is declared once instead — see the
   * fxSetEmbedMinWindow effect below — so the grip simply cannot go below it.
   */
  const report = useCallback(() => {
    frame.current = 0;
    const el = holeRef.current;
    if (!el) return;

    /* Before the first size arrives there is nothing to reserve, and a 0x0 rect
       is not a rectangle — it would be asking C++ to interpret a degenerate case
       that means nothing. Stay silent until the plugin has told us its size. */
    if (wantWidth < 1 || wantHeight < 1) return;

    const r = el.getBoundingClientRect();
    const rect = {
      x: Math.round(r.left),
      y: Math.round(r.top),
      width: Math.round(r.width),
      height: Math.round(r.height),
    };

    const key = `${rect.x},${rect.y},${rect.width},${rect.height}`;
    if (key === sent.current) return;
    sent.current = key;

    // Published for the page as well as for C++: components that position
    // themselves freely (tooltips, toasts) have to be able to avoid the hole.
    setEmbedHole(rect);
    void bridge.fxSetEmbedRect(rect.x, rect.y, rect.width, rect.height);
  }, [wantHeight, wantWidth]);

  /**
   * Declare the smallest window this embed can live in: the plugin's own size plus
   * the chrome the page wraps around it (toolbar, gutters, dock, header, footer).
   * Measured rather than computed from constants, so it cannot drift out of step
   * with the CSS.
   */
  useEffect(() => {
    if (wantWidth < 1 || wantHeight < 1) {
      void bridge.fxSetEmbedMinWindow(0, 0);
      return;
    }

    const hole = holeRef.current;
    if (!hole) return;

    /* Everything the window currently spends on something other than the hole:
       toolbar, gutters, dock, header, footer. Measured, so it cannot drift out of
       step with the CSS the way a table of constants would. */
    const holeRect = hole.getBoundingClientRect();
    const { width, height } = useStore.getState().editorSize;
    const spentX = Math.max(0, width - holeRect.width);
    const spentY = Math.max(0, height - holeRect.height);

    void bridge.fxSetEmbedMinWindow(
      Math.ceil(wantWidth + spentX),
      Math.ceil(wantHeight + spentY),
    );
  }, [wantHeight, wantWidth]);

  const schedule = useCallback(() => {
    if (!frame.current) frame.current = requestAnimationFrame(report);
  }, [report]);

  /* A new reported size means a new rectangle to measure. */
  useEffect(() => {
    schedule();
  }, [schedule, wantHeight, wantWidth]);

  useEffect(() => {
    const el = holeRef.current;
    if (!el) return;
    /* ResizeObserver catches the window and every band around the hole; the
       window listener catches a move that changes nothing's size (there is no
       "position observer"). */
    const observer =
      typeof ResizeObserver === "undefined" ? null : new ResizeObserver(schedule);
    observer?.observe(el);
    window.addEventListener("resize", schedule);
    return () => {
      observer?.disconnect();
      window.removeEventListener("resize", schedule);
      if (frame.current) cancelAnimationFrame(frame.current);
      frame.current = 0;
    };
  }, [schedule]);

  /* The whole point of the overlay registry: one subscriber, one rule. */
  useEffect(() => {
    void bridge.fxSetEmbedVisible(!hidden);
  }, [hidden]);

  useEffect(
    () => () => {
      setEmbedHole(null);
      // Leaving it hidden would silently swallow the next embed, which mounts
      // without knowing why it cannot be seen.
      void bridge.fxSetEmbedVisible(true);
    },
    [],
  );

  const popOut = useCallback(async () => {
    // Unmount first: the same editor cannot be in our window and in its own.
    await unembed();
    await openFxEditor(slot);
  }, [openFxEditor, slot, unembed]);

  return (
    <div className={s.panel}>
      <div className={s.toolbar}>
        <span className={s.name} title={name}>
          {name}
        </span>
        {manufacturer && <span className={s.maker}>{manufacturer}</span>}
        <div className={s.actions}>
          <Button size="sm" variant="ghost" onClick={() => void popOut()}>
            Pop out
          </Button>
          <Button size="sm" variant="ghost" onClick={() => void unembed()}>
            Close
          </Button>
        </div>
      </div>

      <div className={s.area}>
        {/*
          The hole. Its inline size is the hosted editor's own — capped at the
          band, which is what turns "does not fit" into a measurable shortfall.
          Nothing may be rendered on top of it, and nothing rendered inside it is
          visible in the plugin; the two children below are the exceptions that
          prove it, since each only exists when the native view does not.
        */}
        <div
          ref={holeRef}
          className={s.hole}
          style={{ width: wantWidth || undefined, height: wantHeight || undefined }}
        >
          {hidden ? (
            <div className={s.dimmed}>
              <span>{name || "The plugin editor"} is hidden</span>
              <span className={s.dimmedHint}>
                It is drawn above this window and would cover what is open.
              </span>
            </div>
          ) : (
            isMockBridge && (
              <div className={s.standIn}>
                <span>{name}</span>
                <span className={s.standInHint}>
                  {wantWidth} × {wantHeight} — the plugin's own view goes here
                </span>
              </div>
            )
          )}
        </div>
      </div>
    </div>
  );
}
