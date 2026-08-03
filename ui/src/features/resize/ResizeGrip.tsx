/**
 * The window's corner resizer, drawn in the page.
 *
 * It is here rather than in C++ because there is nowhere else for it to be:
 * JUCE's `ResizableCornerComponent` is a JUCE-painted child and the WebView is a
 * native view covering every one of them, so a real corner grip would be
 * invisible. And an AU host cannot resize us either — JUCE's wrapper reverts a
 * host-driven resize on the next `parentSizeChanged` but propagates a
 * plugin-driven one. So the only handle that works is this one, and it drives
 * `setEditorSize` (bridge/types.ts §window size).
 *
 * `editorSizeChanged` is the single source of truth for the current size: the
 * drag sends requests and never assumes they were honoured, which is what makes
 * the clamps at the min/max feel like a window hitting a stop rather than the
 * page arguing with itself.
 */
import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type PointerEvent as ReactPointerEvent,
} from "react";
import { useStore } from "../../store";
import { cx } from "../../components";
import s from "./resize.module.css";

interface DragOrigin {
  pointerId: number;
  x: number;
  y: number;
  width: number;
  height: number;
}

function clamp(value: number, min: number, max: number): number {
  return value < min ? min : value > max ? max : value;
}

export function ResizeGrip() {
  const resizeEditor = useStore((st) => st.resizeEditor);
  const [dragging, setDragging] = useState(false);

  const origin = useRef<DragOrigin | null>(null);
  const pending = useRef<{ width: number; height: number } | null>(null);
  const frame = useRef(0);

  const flush = useCallback(() => {
    frame.current = 0;
    const next = pending.current;
    pending.current = null;
    if (next) resizeEditor(next.width, next.height);
  }, [resizeEditor]);

  useEffect(
    () => () => {
      if (frame.current) cancelAnimationFrame(frame.current);
    },
    [],
  );

  const onPointerDown = useCallback(
    (event: ReactPointerEvent<HTMLDivElement>) => {
      if (event.button !== 0) return;
      // Kills the text-selection drag and any native drag image; the grip is
      // chrome, not content.
      event.preventDefault();
      const { width, height } = useStore.getState().editorSize;
      origin.current = {
        pointerId: event.pointerId,
        x: event.clientX,
        y: event.clientY,
        width,
        height,
      };
      event.currentTarget.setPointerCapture(event.pointerId);
      setDragging(true);
    },
    [],
  );

  const onPointerMove = useCallback(
    (event: ReactPointerEvent<HTMLDivElement>) => {
      const from = origin.current;
      if (!from || event.pointerId !== from.pointerId) return;

      // Deltas from the press, never from the last frame: a clamped edge must
      // not eat travel, or dragging back off the stop would lag by however far
      // past it the pointer went. Limits are re-read every move because the
      // display the window sits on can change under a drag.
      const limits = useStore.getState().editorSize;
      pending.current = {
        width: clamp(
          from.width + (event.clientX - from.x),
          limits.minWidth,
          limits.maxWidth,
        ),
        height: clamp(
          from.height + (event.clientY - from.y),
          limits.minHeight,
          limits.maxHeight,
        ),
      };
      // One request per frame: a pointermove stream is faster than the window
      // can be resized, and every request costs a native round-trip and a
      // relayout of the whole page.
      if (!frame.current) frame.current = requestAnimationFrame(flush);
    },
    [flush],
  );

  const endDrag = useCallback(
    (event: ReactPointerEvent<HTMLDivElement>) => {
      const from = origin.current;
      if (!from || event.pointerId !== from.pointerId) return;
      origin.current = null;
      setDragging(false);
      if (frame.current) {
        cancelAnimationFrame(frame.current);
        flush();
      }
    },
    [flush],
  );

  return (
    <div
      className={cx(s.grip, dragging && s.gripActive)}
      role="button"
      aria-label="Resize the plugin window"
      title="Drag to resize"
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={endDrag}
      onPointerCancel={endDrag}
      onLostPointerCapture={endDrag}
    >
      {/* Three rules stepping out of the corner — the shape every platform's
          resizer uses, so it reads as one without a label. */}
      <svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true">
        <path
          d="M14 6 L6 14 M14 10 L10 14 M14 2 L2 14"
          stroke="currentColor"
          strokeWidth={1.25}
          strokeLinecap="round"
        />
      </svg>
    </div>
  );
}
