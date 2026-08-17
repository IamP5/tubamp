/**
 * The reverb body's 18px hover info strip — an in-flow element, deliberately
 * NOT a Tooltip: no portal, no hover delay, no embed-hole interaction. Its
 * three spans are painted by `useReverbInfo` via direct `textContent` writes
 * (never React state), so it is `role="note"` with no `aria-live` — the
 * controls' own aria-labels carry accessibility.
 */
import type { RefObject } from "react";
import s from "./panel.module.css";

export interface ReverbInfoStripProps {
  keyRef: RefObject<HTMLSpanElement | null>;
  textRef: RefObject<HTMLSpanElement | null>;
  rangeRef: RefObject<HTMLSpanElement | null>;
}

export function ReverbInfoStrip({ keyRef, textRef, rangeRef }: ReverbInfoStripProps) {
  return (
    <div className={s.rvStrip} role="note">
      <span ref={keyRef} className={s.rvStripKey} />
      <span ref={textRef} className={s.rvStripText} />
      <span ref={rangeRef} className={s.rvStripRange} />
    </div>
  );
}
