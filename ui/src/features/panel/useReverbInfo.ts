/**
 * Delegated hover → info-strip painter for the reverb body. ZERO React renders
 * per hover: one `pointerover`/`focusin`/`pointerleave` listener set on the
 * body root, `closest("[data-rv-key]")` to resolve the hovered control, and
 * three `textContent` writes into the strip's spans — the same
 * direct-DOM-write discipline the Knob/ParamSlider readouts follow
 * (motion-design.md §4.4).
 *
 * Greyed controls still report: `.knobDisabled` / `.fieldDisabled` set
 * `pointer-events: none` on the CONTROL, so hover events target the wrapper —
 * which is what carries `data-rv-key`. That is why those shared classes are
 * never edited for this feature.
 */
import { useCallback, useEffect, useRef, type RefObject } from "react";
import type { BlockId } from "../../bridge";
import { reverbInfoLine, reverbModeLine, type RvInfoKey, type RvInfoLine } from "./reverbInfo";

export function useReverbInfo(
  rootRef: RefObject<HTMLDivElement | null>,
  keyRef: RefObject<HTMLSpanElement | null>,
  textRef: RefObject<HTMLSpanElement | null>,
  rangeRef: RefObject<HTMLSpanElement | null>,
  block: BlockId,
  algo: number,
): void {
  /* Kept current each render so the delegated listeners (attached once) always
     resolve against the live mode/instance. */
  const currentRef = useRef({ block, algo });

  const paint = useCallback(
    (line: RvInfoLine): void => {
      if (keyRef.current) keyRef.current.textContent = line.key;
      if (textRef.current) textRef.current.textContent = line.text;
      if (rangeRef.current) rangeRef.current.textContent = line.range ?? "";
    },
    [keyRef, textRef, rangeRef],
  );

  /* Idle repaint: the strip is never blank — it shows the current mode's blurb
     whenever nothing is hovered, and re-paints on every algo/instance change. */
  useEffect(() => {
    currentRef.current = { block, algo };
    paint(reverbModeLine(algo));
  }, [block, algo, paint]);

  useEffect(() => {
    const root = rootRef.current;
    if (!root) return;

    const resolve = (target: EventTarget | null): RvInfoLine | null => {
      if (!(target instanceof Element)) return null;
      const { block: liveBlock, algo: liveAlgo } = currentRef.current;
      /* Mode-segment hover previews the segment's own mode blurb BEFORE the
         user commits. Resolution depends on kit Segmented rendering its
         buttons as direct children of its root — verified; do not wrap them. */
      const bar = target.closest("[data-rv-modebar]");
      if (bar) {
        const button = target.closest("button");
        if (button) {
          const index = Array.prototype.indexOf.call(
            bar.querySelectorAll("button"),
            button,
          );
          if (index >= 0) return reverbModeLine(index);
        }
        /* Over the bar's own padding/gap rather than a segment: repaint the
           idle line so a stale preview never sticks to the strip. */
        return reverbModeLine(liveAlgo);
      }
      const keyed = target.closest("[data-rv-key]");
      if (!keyed) return null;
      const key = keyed.getAttribute("data-rv-key") as RvInfoKey | null;
      if (!key) return null;
      return reverbInfoLine(key, liveAlgo, liveBlock);
    };

    const onOver = (e: Event): void => {
      const line = resolve(e.target);
      if (line) paint(line);
    };
    /* pointerleave (not pointerout): fires only when leaving the whole body,
       never when crossing into a child. During a knob drag the pointer is
       captured by the control, so the strip stays on the dragged control. */
    const onLeave = (): void => paint(reverbModeLine(currentRef.current.algo));

    root.addEventListener("pointerover", onOver);
    root.addEventListener("focusin", onOver);
    root.addEventListener("pointerleave", onLeave);
    return () => {
      root.removeEventListener("pointerover", onOver);
      root.removeEventListener("focusin", onOver);
      root.removeEventListener("pointerleave", onLeave);
    };
  }, [rootRef, paint]);
}
