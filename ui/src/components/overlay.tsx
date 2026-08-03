/**
 * Overlay bookkeeping — one module, because the thing it protects is invisible.
 *
 * A hosted plugin's editor is a NATIVE view composited ABOVE the WebView (see
 * bridge/types.ts §embedding). The page cannot draw over the rectangle it
 * occupies: whatever we render there is simply not on screen. Two consequences,
 * and both of them are cross-cutting, so they live here rather than in the
 * feature that owns the embed:
 *
 *  1. Anything that must appear ON TOP of the page — a menu with its scrim, a
 *     modal, the T3K drawer — has to have the native view hidden first.
 *     Overlays therefore only *declare* themselves (`useBlockingOverlay`, or by
 *     rendering a `<Scrim>`); features/embed subscribes once and does the
 *     hiding. Per-overlay hide/restore code would be forgotten by the next
 *     overlay someone adds.
 *  2. Anything that positions itself freely — today only the tooltip — has to
 *     be able to ask where the hole is, so it can land somewhere visible.
 *     features/embed publishes it here with `setEmbedHole`.
 *
 * Nothing in this module talks to the bridge; it is a registry, not a policy.
 */
import { useEffect, useSyncExternalStore, type ComponentProps } from "react";

/* ─────────────────────── blocking overlays (a count) ───────────────────── */

/**
 * A count, not a boolean: a menu opened from inside a modal must not restore the
 * native view when only the menu closes.
 */
let openCount = 0;
const overlayListeners = new Set<(open: boolean) => void>();

function publishOverlays(): void {
  const open = openCount > 0;
  for (const fn of [...overlayListeners]) fn(open);
}

export function blockingOverlaysOpen(): boolean {
  return openCount > 0;
}

export function onBlockingOverlaysChange(
  listener: (open: boolean) => void,
): () => void {
  overlayListeners.add(listener);
  return () => overlayListeners.delete(listener);
}

/** Declares "something of mine is drawn above the page" for as long as `active`. */
export function useBlockingOverlay(active = true): void {
  useEffect(() => {
    if (!active) return;
    openCount += 1;
    publishOverlays();
    return () => {
      openCount -= 1;
      publishOverlays();
    };
  }, [active]);
}

/**
 * The click-catching layer under a menu or dialog. It is a plain div plus the
 * declaration above — every overlay that needs one gets registered by using it,
 * which is the whole reason this exists as a component instead of a class name.
 */
export function Scrim(props: ComponentProps<"div">) {
  useBlockingOverlay(true);
  return <div {...props} />;
}

/* ──────────────────────────── the embed hole ───────────────────────────── */

export interface HoleRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

let hole: HoleRect | null = null;
const holeListeners = new Set<() => void>();

/** Published by features/embed whenever the reserved rectangle moves. */
export function setEmbedHole(rect: HoleRect | null): void {
  const same =
    hole === rect ||
    (hole !== null &&
      rect !== null &&
      hole.x === rect.x &&
      hole.y === rect.y &&
      hole.width === rect.width &&
      hole.height === rect.height);
  if (same) return;
  hole = rect;
  for (const listener of holeListeners) listener();
}

/** Where the native hosted editor is, in client coordinates; null when none.
 *  Imperative form, for callers that only need it at an interaction. */
export function embedHole(): HoleRect | null {
  return hole;
}

function subscribeHole(listener: () => void): () => void {
  holeListeners.add(listener);
  return () => holeListeners.delete(listener);
}

/** Reactive form, for components that must lay themselves out around the hole.
 *  The rect object is replaced rather than mutated, so identity is a valid
 *  snapshot for useSyncExternalStore. */
export function useEmbedHole(): HoleRect | null {
  return useSyncExternalStore(subscribeHole, embedHole, () => null);
}
