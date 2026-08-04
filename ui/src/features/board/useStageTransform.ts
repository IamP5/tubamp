/**
 * Pan / zoom for the board stage (motion-design.md §2.13).
 *
 * The whole transform lives in three motion values applied to ONE stage div
 * (`translate(panX, panY) scale(zoom)`, transform-origin = the stage centre),
 * so no gesture ever re-renders React. Zoom is clamped at the source rather than
 * by fighting an out-of-range spring afterwards.
 *
 * Gestures:
 *   wheel                → zoom to cursor
 *   ctrl/cmd + wheel     → fine zoom to cursor (trackpad pinch arrives this way)
 *   shift + wheel, or a wheel event with a horizontal component (trackpad
 *                          two-finger swipe) → pan
 *   space-drag / middle-drag / drag on empty stage → pan (with release inertia)
 *   double-click on empty stage → animated zoom-to-fit
 *   tap on empty stage   → `onBackgroundTap` (the board dismisses the panel)
 *
 * The stage box is the FULL container: the param panel is an overlay, so the
 * board paints under it and the transform origin never moves. `insetBottom` is
 * how much of that box the panel currently covers, and it only enters the
 * framing decisions — fit, the pan leash, and ensure-visible.
 */
import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type MouseEvent as ReactMouseEvent,
  type PointerEvent as ReactPointerEvent,
  type RefObject,
} from "react";
import { animate, useMotionValue, type MotionValue } from "motion/react";
import { spring } from "../../theme/motion";
import { clamp, type Box } from "./layout";

/*
  0.3, derived the way 0.4 was derived for the single-lane board.

  The auto-wrapped worst case is 24 blocks at ROW_WRAP = 6: 1204 lane units
  wide (last row also carries [+] and OUT) and 544 tall. The minimum window
  (900×600) leaves the board 900×496, and zoom-to-fit keeps FIT_PADDING around
  the content, so framing it needs only max(1204/828, 544/424) → 0.688.

  Hand-built rows can be wider than the wrap, so the floor is set by what a
  user can reasonably line up in ONE row instead: sixteen blocks are 2720 lane
  units, which needs 828/2720 = 0.304. Beyond that FIT saturates and panning
  takes over — at 0.3 a card is already only 34px wide.
*/
export const MIN_ZOOM = 0.3;
export const MAX_ZOOM = 1.6;

/** Wheel-delta → zoom-factor exponents (fine = ctrl/cmd, i.e. trackpad pinch). */
const ZOOM_RATE = 0.0022;
const ZOOM_RATE_FINE = 0.0007;

/** Margin left around the content by zoom-to-fit, in CSS px. */
const FIT_PADDING = 72;

/** Screen px of travel a background press may have and still count as a tap. */
const TAP_TRAVEL_PX = 4;

/**
 * How long a background tap waits before it is announced.
 *
 * The dismiss and the zoom-to-fit share one surface and one first press, and
 * "double-click never dismisses" can only be honoured by outliving the second
 * press. Kept as short as a double-click plausibly is, so the panel still
 * closes as an immediate-feeling response to a click on the board.
 */
const TAP_DOUBLE_WINDOW_MS = 240;

/** Air kept between an ensure-visible target and the edge it was pulled from. */
const REVEAL_MARGIN = 24;

const INSTANT = { duration: 0 } as const;

export interface StageTransformOptions {
  /** Bounding box of everything on the board, in lane units. */
  content: Box;
  reduced: boolean;
  /**
   * CSS px of the container's bottom edge covered by the param panel — the
   * panel band while a block is selected, 0 otherwise. The board paints
   * full-bleed (the dot grid runs behind and around the panel card), but every
   * framing decision is taken against the *visible* band above it, so a chain
   * never settles underneath the panel.
   */
  insetBottom?: number;
  /**
   * A press-and-release on the stage background that neither panned nor turned
   * into a double-click. The board wires this to "deselect" (R4): dismissing on
   * pointerdown would close the panel at the start of every empty-stage pan and
   * on the first half of every double-click-to-fit.
   */
  onBackgroundTap?(): void;
}

export interface StageTransform {
  containerRef: RefObject<HTMLDivElement | null>;
  panX: MotionValue<number>;
  panY: MotionValue<number>;
  zoom: MotionValue<number>;
  /** True while space is held or a pan gesture is running — cards must not drag. */
  isPanMode(): boolean;
  panMode: boolean;
  panning: boolean;
  onPointerDown(event: ReactPointerEvent<HTMLDivElement>): void;
  onDoubleClick(event: ReactMouseEvent<HTMLDivElement>): void;
  zoomBy(factor: number): void;
  zoomTo(next: number): void;
  fit(animated?: boolean): void;
  /** Pan the minimum needed to bring `box` (lane units) into the visible band. */
  ensureVisible(box: Box): void;
}

export function useStageTransform({
  content,
  reduced,
  insetBottom = 0,
  onBackgroundTap,
}: StageTransformOptions): StageTransform {
  const containerRef = useRef<HTMLDivElement | null>(null);
  const panX = useMotionValue(0);
  const panY = useMotionValue(0);
  const zoom = useMotionValue(1);

  // Mirrored so `fit()` can stay identity-stable across content changes.
  const contentRef = useRef(content);
  useLayoutEffect(() => {
    contentRef.current = content;
  }, [content]);

  const tapRef = useRef(onBackgroundTap);
  useLayoutEffect(() => {
    tapRef.current = onBackgroundTap;
  }, [onBackgroundTap]);

  const spaceRef = useRef(false);
  const panningRef = useRef(false);
  const tapTimer = useRef<number | null>(null);
  const lastTapAt = useRef(-Infinity);
  const [panMode, setPanMode] = useState(false);
  const [panning, setPanning] = useState(false);

  /**
   * True once the user has zoomed or panned by hand.
   *
   * It decides what a window resize means. Until the user has touched the
   * canvas, the framing is ours and re-fitting on resize is simply keeping the
   * promise the initial fit made — the chain stays framed as the window grows,
   * which is the whole point of a resizable editor. After they have taken
   * control, re-fitting would yank the canvas out from under them on every
   * frame of a grip drag, so a resize then only re-clamps the pan into the new
   * bounds. "FIT" hands control back and re-arms the automatic framing.
   */
  const userAdjusted = useRef(false);

  /** Height of the band the panel does not cover. */
  const viewHeight = useCallback(
    (el: HTMLDivElement) => Math.max(el.clientHeight - insetBottom, 1),
    [insetBottom],
  );

  /**
   * Pan leash. Half the viewport plus half the (scaled) content: at the limit
   * the content's near edge has just reached the far edge of the viewport, so
   * every row of a tall chain is reachable and none of them can be thrown away.
   * A fixed fraction of the container — what a single-row lane could get away
   * with — would leave the bottom rows of a wrapped chain unreachable.
   */
  const panLimit = useCallback(
    (axis: "x" | "y"): number => {
      const el = containerRef.current;
      if (!el) return 240;
      const half = (axis === "x" ? el.clientWidth : viewHeight(el)) / 2;
      const box = contentRef.current;
      const extent = (axis === "x" ? box.w : box.h) * zoom.get();
      return half + extent / 2;
    },
    [viewHeight, zoom],
  );

  const setPan = useCallback(
    (x: number, y: number) => {
      panX.set(clamp(x, -panLimit("x"), panLimit("x")));
      panY.set(clamp(y, -panLimit("y"), panLimit("y")));
    },
    [panLimit, panX, panY],
  );

  /* ───────────────────────────── zoom helpers ──────────────────────────── */

  /**
   * Zoom keeping the world point under (clientX, clientY) pinned.
   *
   * With `translate(p) scale(z)` about the container centre `c`, a world point
   * `w` lands at `c + p + z·w`, so holding the screen point fixed while z → z'
   * means `p' = p + (z − z')·w`, with `w = (screen − c − p) / z`.
   */
  const zoomAt = useCallback(
    (next: number, clientX: number, clientY: number) => {
      const el = containerRef.current;
      if (!el) return;
      const rect = el.getBoundingClientRect();
      // Must match the stage element's transform-origin, which is the centre of
      // the whole container (the panel is an overlay, not a layout band).
      const cx = rect.left + rect.width / 2;
      const cy = rect.top + rect.height / 2;
      const z = zoom.get();
      const target = clamp(next, MIN_ZOOM, MAX_ZOOM);
      if (target === z) return;
      userAdjusted.current = true;
      const wx = (clientX - cx - panX.get()) / z;
      const wy = (clientY - cy - panY.get()) / z;
      zoom.set(target);
      setPan(panX.get() + (z - target) * wx, panY.get() + (z - target) * wy);
    },
    [panX, panY, setPan, zoom],
  );

  /**
   * Zoom about the viewport centre. Pan scales with the zoom ratio, otherwise
   * the point currently under the centre would drift as the scale changes.
   */
  const zoomTo = useCallback(
    (next: number) => {
      userAdjusted.current = true;
      const from = zoom.get();
      const target = clamp(next, MIN_ZOOM, MAX_ZOOM);
      if (target === from) return;
      const ratio = target / from;
      const options = reduced ? INSTANT : spring.canvas;
      const limitX = panLimit("x");
      const limitY = panLimit("y");
      animate(zoom, target, options);
      // These bypass setPan, so re-clamp here: a saturated pan scaled by the zoom
      // ratio would otherwise leave the canvas outside the allowed range until
      // the next drag snapped it back in one frame.
      animate(panX, clamp(panX.get() * ratio, -limitX, limitX), options);
      animate(panY, clamp(panY.get() * ratio, -limitY, limitY), options);
    },
    [panLimit, panX, panY, reduced, zoom],
  );

  const zoomBy = useCallback(
    (factor: number) => zoomTo(zoom.get() * factor),
    [zoom, zoomTo],
  );

  /**
   * Frame the content's own centre — with rows, the board's bounding box is
   * neither centred on the lane origin nor symmetric about it, so "pan back to
   * 0" (what a single centred lane could do) would frame the origin and crop
   * the bottom rows. The target lands the box centre on the centre of the band
   * the panel leaves visible.
   */
  const fit = useCallback(
    (animated = true) => {
      const el = containerRef.current;
      if (!el) return;
      const box = contentRef.current;
      if (box.w <= 0 || box.h <= 0) return;
      userAdjusted.current = false;
      const target = clamp(
        Math.min(
          (el.clientWidth - FIT_PADDING) / box.w,
          (viewHeight(el) - FIT_PADDING) / box.h,
        ),
        MIN_ZOOM,
        MAX_ZOOM,
      );
      const cx = box.x + box.w / 2;
      const cy = box.y + box.h / 2;
      const options = animated && !reduced ? spring.canvas : INSTANT;
      animate(zoom, target, options);
      animate(panX, -target * cx, options);
      animate(panY, -target * cy - insetBottom / 2, options);
    },
    [insetBottom, panX, panY, reduced, viewHeight, zoom],
  );

  /**
   * Minimal pan that brings `box` inside the visible band. Used when a block is
   * selected: the panel is an overlay, so a card in the bottom row would open
   * its own panel on top of itself.
   */
  const ensureVisible = useCallback(
    (box: Box) => {
      const el = containerRef.current;
      if (!el) return;
      const z = zoom.get();
      // Everything below is measured from the container centre, which is where
      // the transform's origin is.
      const halfW = el.clientWidth / 2;
      const halfH = el.clientHeight / 2;
      const left = -halfW + REVEAL_MARGIN;
      const right = halfW - REVEAL_MARGIN;
      const top = -halfH + REVEAL_MARGIN;
      const bottom = halfH - insetBottom - REVEAL_MARGIN;

      let x = panX.get();
      let y = panY.get();
      const boxLeft = z * box.x;
      const boxRight = z * (box.x + box.w);
      const boxTop = z * box.y;
      const boxBottom = z * (box.y + box.h);

      // Overflow first, underflow second: a box taller than the band is pinned
      // to the band's top rather than to its (invisible) bottom.
      if (x + boxRight > right) x = right - boxRight;
      if (x + boxLeft < left) x = left - boxLeft;
      if (y + boxBottom > bottom) y = bottom - boxBottom;
      if (y + boxTop < top) y = top - boxTop;

      if (x === panX.get() && y === panY.get()) return;
      const limitX = panLimit("x");
      const limitY = panLimit("y");
      const options = reduced ? INSTANT : spring.canvas;
      animate(panX, clamp(x, -limitX, limitX), options);
      animate(panY, clamp(y, -limitY, limitY), options);
    },
    [insetBottom, panLimit, panX, panY, reduced, zoom],
  );

  /* Frame the content once, when the editor opens. */
  const framed = useRef(false);
  useEffect(() => {
    if (framed.current) return;
    framed.current = true;
    fit(false);
  }, [fit]);

  /**
   * The stage is now fluid, so its size changes under a running transform: the
   * user drags the window's grip, opens the T3K drawer, or the host hands us a
   * different size at startup.
   *
   * Two responses, both required:
   *  - the pan leash is measured against the container, so a shrink leaves the
   *    pan outside its own bounds until the next gesture snaps it back in one
   *    jump; re-clamp every time.
   *  - re-fit, but only while the framing is still ours (see `userAdjusted`),
   *    and never animated: this fires once per frame of a grip drag, and a
   *    spring chasing a target that moves every frame reads as lag, not motion.
   */
  useEffect(() => {
    const el = containerRef.current;
    if (!el || typeof ResizeObserver === "undefined") return;
    /* Skip the observer's initial callback — the first fit already ran. */
    let first = true;
    const observer = new ResizeObserver(() => {
      if (first) {
        first = false;
        return;
      }
      if (userAdjusted.current) setPan(panX.get(), panY.get());
      else fit(false);
    });
    observer.observe(el);
    return () => observer.disconnect();
  }, [fit, panX, panY, setPan]);

  /* ───────────────────────────────── wheel ─────────────────────────────── */

  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    const onWheel = (event: WheelEvent): void => {
      // Non-passive: the WebView would otherwise rubber-band the whole page.
      event.preventDefault();
      const fine = event.ctrlKey || event.metaKey;
      const isPanGesture =
        !fine && (event.shiftKey || Math.abs(event.deltaX) > 0.5);
      if (isPanGesture) {
        userAdjusted.current = true;
        setPan(panX.get() - event.deltaX, panY.get() - event.deltaY);
        return;
      }
      const rate = fine ? ZOOM_RATE_FINE : ZOOM_RATE;
      zoomAt(
        zoom.get() * Math.exp(-event.deltaY * rate),
        event.clientX,
        event.clientY,
      );
    };
    el.addEventListener("wheel", onWheel, { passive: false });
    return () => el.removeEventListener("wheel", onWheel);
  }, [panX, panY, setPan, zoom, zoomAt]);

  /* ─────────────────────────────── space key ───────────────────────────── */

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent): void => {
      if (event.code !== "Space" || event.repeat) return;
      // Space is also the activation key for every button/switch in this UI, and
      // this listener is on window: preventDefault here would suppress the
      // synthesised click for whatever has focus. Only claim Space when focus is
      // not on something that acts on it.
      const target = event.target;
      if (
        target instanceof Element &&
        target.closest("button, input, select, textarea, [role='menuitem']")
      )
        return;
      event.preventDefault();
      spaceRef.current = true;
      setPanMode(true);
    };
    const onKeyUp = (event: KeyboardEvent): void => {
      if (event.code !== "Space") return;
      spaceRef.current = false;
      setPanMode(false);
    };
    const onBlur = (): void => {
      spaceRef.current = false;
      setPanMode(false);
    };
    window.addEventListener("keydown", onKeyDown);
    window.addEventListener("keyup", onKeyUp);
    window.addEventListener("blur", onBlur);
    return () => {
      window.removeEventListener("keydown", onKeyDown);
      window.removeEventListener("keyup", onKeyUp);
      window.removeEventListener("blur", onBlur);
    };
  }, []);

  /* ──────────────────────── background tap bookkeeping ─────────────────── */

  const cancelTap = useCallback(() => {
    if (tapTimer.current === null) return;
    window.clearTimeout(tapTimer.current);
    tapTimer.current = null;
  }, []);

  /**
   * Any press anywhere supersedes a dismiss that has not fired yet — the second
   * half of a double-click, or a card being picked up right after a background
   * click. Capture phase, because a card's own handler stops propagation.
   */
  useEffect(() => {
    window.addEventListener("pointerdown", cancelTap, true);
    return () => {
      window.removeEventListener("pointerdown", cancelTap, true);
      cancelTap();
    };
  }, [cancelTap]);

  /* ──────────────────────────────── panning ────────────────────────────── */

  const onPointerDown = useCallback(
    (event: ReactPointerEvent<HTMLDivElement>) => {
      const target = event.target as HTMLElement;
      const onBackground = target.dataset.stageBg !== undefined;
      const middle = event.button === 1;
      if (!middle && !spaceRef.current && !onBackground) return;
      if (event.button !== 0 && !middle) return;
      event.preventDefault();

      // A press that lands inside the previous tap's window is the back half of
      // a double-click (zoom-to-fit): the capture listener above has already
      // dropped the pending dismiss, and this gesture must not arm another.
      const tappable =
        onBackground &&
        !middle &&
        event.detail < 2 &&
        performance.now() - lastTapAt.current > TAP_DOUBLE_WINDOW_MS;

      const el = event.currentTarget;
      el.setPointerCapture(event.pointerId);
      panningRef.current = true;
      setPanning(true);

      const startX = event.clientX;
      const startY = event.clientY;
      const originX = panX.get();
      const originY = panY.get();
      let lastX = startX;
      let lastY = startY;
      let lastT = performance.now();
      let travel = 0;
      let vx = 0;
      let vy = 0;

      const onMove = (move: PointerEvent): void => {
        if (move.pointerId !== event.pointerId) return;
        const now = performance.now();
        const dt = Math.max(now - lastT, 1) / 1000;
        vx = (move.clientX - lastX) / dt;
        vy = (move.clientY - lastY) / dt;
        lastX = move.clientX;
        lastY = move.clientY;
        lastT = now;
        travel = Math.max(
          travel,
          Math.abs(move.clientX - startX),
          Math.abs(move.clientY - startY),
        );
        // Only a press that actually pans gives up the automatic framing — a
        // zero-travel dismiss tap must leave resize-refit armed.
        if (travel > TAP_TRAVEL_PX) userAdjusted.current = true;
        setPan(originX + (move.clientX - startX), originY + (move.clientY - startY));
      };

      const finish = (): void => {
        window.removeEventListener("pointermove", onMove);
        window.removeEventListener("pointerup", finish);
        window.removeEventListener("pointercancel", finish);
        panningRef.current = false;
        setPanning(false);

        if (tappable && travel <= TAP_TRAVEL_PX) {
          // Deliberately deferred: see TAP_DOUBLE_WINDOW_MS.
          lastTapAt.current = performance.now();
          tapTimer.current = window.setTimeout(() => {
            tapTimer.current = null;
            tapRef.current?.();
          }, TAP_DOUBLE_WINDOW_MS);
        }

        if (reduced) return;
        // The one intentional soft-bounce in the app: momentum on a big surface.
        const inertia = {
          type: "inertia" as const,
          power: 0.7,
          timeConstant: 300,
          bounceStiffness: 300,
          bounceDamping: 40,
        };
        if (Math.abs(vx) > 40) {
          animate(panX, panX.get(), {
            ...inertia,
            velocity: vx,
            min: -panLimit("x"),
            max: panLimit("x"),
          });
        }
        if (Math.abs(vy) > 40) {
          animate(panY, panY.get(), {
            ...inertia,
            velocity: vy,
            min: -panLimit("y"),
            max: panLimit("y"),
          });
        }
      };

      window.addEventListener("pointermove", onMove);
      window.addEventListener("pointerup", finish);
      window.addEventListener("pointercancel", finish);
    },
    [panLimit, panX, panY, reduced, setPan],
  );

  const onDoubleClick = useCallback(
    (event: ReactMouseEvent<HTMLDivElement>) => {
      const target = event.target as HTMLElement;
      if (target.dataset.stageBg === undefined) return;
      // Belt and braces: the second pointerdown already dropped it.
      cancelTap();
      fit(true);
    },
    [cancelTap, fit],
  );

  const isPanMode = useCallback(
    () => spaceRef.current || panningRef.current,
    [],
  );

  return {
    containerRef,
    panX,
    panY,
    zoom,
    isPanMode,
    panMode,
    panning,
    onPointerDown,
    onDoubleClick,
    zoomBy,
    zoomTo,
    fit,
    ensureVisible,
  };
}
