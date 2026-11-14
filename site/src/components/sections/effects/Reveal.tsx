"use client";

import { useRef } from "react";
import { useGSAP } from "@gsap/react";
import { gsap, MQ, setupGsap } from "@/lib/motion";

/**
 * Reveals every `[data-line]` inside it, in order, once it scrolls into view.
 * A line is a mask (`overflow-hidden`) around one inner element that rises
 * into it, so the text arrives already moving and lands without a fade tail.
 * Under reduced motion nothing is touched and the text sits in place.
 */
export default function Reveal({
  children,
  className,
  start = "top 82%",
  stagger = 0.09,
}: {
  children: React.ReactNode;
  className?: string;
  start?: string;
  stagger?: number;
}) {
  const scope = useRef<HTMLDivElement>(null);

  useGSAP(
    () => {
      setupGsap();
      const mm = gsap.matchMedia();
      mm.add(MQ.motion, () => {
        const inner = gsap.utils.toArray<HTMLElement>("[data-line] > *", scope.current);
        gsap.from(inner, {
          yPercent: 110,
          duration: 1.1,
          stagger,
          scrollTrigger: { trigger: scope.current, start, once: true },
        });
      });
      return () => mm.revert();
    },
    { scope },
  );

  return (
    <div ref={scope} className={className}>
      {children}
    </div>
  );
}

/** One masked line for `Reveal`. The padding keeps descenders out of the mask's clip. */
export function Line({ children, className = "" }: { children: React.ReactNode; className?: string }) {
  return (
    <span data-line className="-mb-[0.12em] block overflow-hidden pb-[0.12em]">
      <span className={`block ${className}`}>{children}</span>
    </span>
  );
}
