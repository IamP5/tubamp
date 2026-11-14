"use client";

import { useRef } from "react";
import { useGSAP } from "@gsap/react";
import { gsap, MQ, setupGsap } from "@/lib/motion";

/**
 * The amber field arriving: it enters as an inset panel and opens to the full
 * bleed as its top climbs the viewport, while anything marked `data-rise`
 * travels up into place. Reduced motion gets the open field.
 */
export default function Curtain({ children, className = "" }: { children: React.ReactNode; className?: string }) {
  const ref = useRef<HTMLDivElement>(null);

  useGSAP(
    () => {
      setupGsap();
      const mm = gsap.matchMedia();
      mm.add(MQ.motion, () => {
        const el = ref.current!;
        const scrollTrigger = { trigger: el, start: "top bottom", end: "top 10%", scrub: true };
        gsap.fromTo(
          el,
          { clipPath: "inset(14% 6% 0% 6% round 40px)" },
          { clipPath: "inset(0% 0% 0% 0% round 0px)", ease: "none", scrollTrigger },
        );
        gsap.fromTo(
          el.querySelectorAll("[data-rise]"),
          { yPercent: 45 },
          { yPercent: 0, ease: "none", scrollTrigger },
        );
      });
      return () => mm.revert();
    },
    { scope: ref },
  );

  return (
    <div ref={ref} className={className}>
      {children}
    </div>
  );
}
