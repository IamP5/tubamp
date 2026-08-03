/**
 * Pan / zoom for the board stage (motion-design.md §2.13).
 *
 * The whole transform lives in three motion values applied to ONE stage div
 * (`translate(panX, panY) scale(zoom)`, transform-origin = the viewport centre),
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
import { clamp } from "./layout";

export const MIN_ZOOM = 0.5;
export const MAX_ZOOM = 1.6;

/** Wheel-delta → zoom-factor exponents (fine = ctrl/cmd, i.e. trackpad pinch). */
const ZOOM_RATE = 0.0022;
const ZOOM_RATE_FINE = 0.0007;

/** Margin left around the content by zoom-to-fit, in CSS px. */
const FIT_PADDING = 72;

const INSTANT = { duration: 0 } as const;

export interface ContentSize {
  w: number;
  h: number;
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
}

/**
 * @param insetBottom  CSS px of the container's bottom edge covered by the param
 *   dock. The board paints full-bleed (the dot grid runs behind and around the
 *   dock), but every geometric decision — the transform origin, zoom-to-cursor,
 *   zoom-to-fit and the pan clamp — is taken against the *visible* band above
 *   the dock, so the chain never settles underneath it.
 */
export function useStageTransform(
  content: ContentSize,
  reduced: boolean,
  insetBottom = 0,
): StageTransform {
  const containerRef = useRef<HTMLDivElement | null>(null);
  const panX = useMotionValue(0);
  const panY = useMotionValue(0);
  const zoom = useMotionValue(1);

  // Mirrored so `fit()` can stay identity-stable across content changes.
  const contentRef = useRef(content);
  useLayoutEffect(() => {
    contentRef.current = content;
  }, [content]);

  const spaceRef = useRef(false);
  const panningRef = useRef(false);
  const [panMode, setPanMode] = useState(false);
  const [panning, setPanning] = useState(false);

  /** Height of the band the dock does not cover. */
  const viewHeight = useCallback(
    (el: HTMLDivElement) => Math.max(el.clientHeight - insetBottom, 1),
    [insetBottom],
  );

  const panLimit = useCallback(
    (axis: "x" | "y"): number => {
      const el = containerRef.current;
      if (!el) return 240;
      return (axis === "x" ? el.clientWidth : viewHeight(el)) * 0.6;
    },
    [viewHeight],
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
      const cx = rect.left + rect.width / 2;
      // Must match the stage element's transform-origin, which is the centre of
      // the visible band (the stage box stops at the dock's top edge).
      const cy = rect.top + (rect.height - insetBottom) / 2;
      const z = zoom.get();
      const target = clamp(next, MIN_ZOOM, MAX_ZOOM);
      if (target === z) return;
      const wx = (clientX - cx - panX.get()) / z;
      const wy = (clientY - cy - panY.get()) / z;
      zoom.set(target);
      setPan(panX.get() + (z - target) * wx, panY.get() + (z - target) * wy);
    },
    [insetBottom, panX, panY, setPan, zoom],
  );

  /**
   * Zoom about the viewport centre. Pan scales with the zoom ratio, otherwise
   * the point currently under the centre would drift as the scale changes.
   */
  const zoomTo = useCallback(
    (next: number) => {
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

  const fit = useCallback(
    (animated = true) => {
      const el = containerRef.current;
      if (!el) return;
      const { w, h } = contentRef.current;
      if (w <= 0 || h <= 0) return;
      const target = clamp(
        Math.min(
          (el.clientWidth - FIT_PADDING) / w,
          (viewHeight(el) - FIT_PADDING) / h,
        ),
        MIN_ZOOM,
        MAX_ZOOM,
      );
      // The lane is centred on the origin by construction, so fitting is always
      // "pan back to 0, scale to fit".
      const options = animated && !reduced ? spring.canvas : INSTANT;
      animate(zoom, target, options);
      animate(panX, 0, options);
      animate(panY, 0, options);
    },
    [panX, panY, reduced, viewHeight, zoom],
  );

  /* Frame the content once, when the editor opens. */
  const framed = useRef(false);
  useEffect(() => {
    if (framed.current) return;
    framed.current = true;
    fit(false);
  }, [fit]);

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

  /* ──────────────────────────────── panning ────────────────────────────── */

  const onPointerDown = useCallback(
    (event: ReactPointerEvent<HTMLDivElement>) => {
      const target = event.target as HTMLElement;
      const onBackground = target.dataset.stageBg !== undefined;
      const middle = event.button === 1;
      if (!middle && !spaceRef.current && !onBackground) return;
      if (event.button !== 0 && !middle) return;
      event.preventDefault();

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
        setPan(originX + (move.clientX - startX), originY + (move.clientY - startY));
      };

      const finish = (): void => {
        window.removeEventListener("pointermove", onMove);
        window.removeEventListener("pointerup", finish);
        window.removeEventListener("pointercancel", finish);
        panningRef.current = false;
        setPanning(false);
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
      fit(true);
    },
    [fit],
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
  };
}
