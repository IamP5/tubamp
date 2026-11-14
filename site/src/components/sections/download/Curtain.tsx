"use client";

import { useRef } from "react";
import { useGSAP } from "@gsap/react";
import { gsap, MQ, setupGsap } from "@/lib/motion";

export default function Curtain({ children, className = "" }: { children: React.ReactNode; className?: string }) {
  const ref = useRef<HTMLDivElement>(null);

  useGSAP(
    () => {
      setupGsap();
      const mm = gsap.matchMedia();
      mm.add(MQ.motion, () => {
        const el = ref.current!;
        const scrollTrigger = { trigger: el, start: "top bottom", end: "top 10%", scrub: true };
        gsap.fromTo(el.querySelectorAll("[data-rise]"), { yPercent: 45 }, { yPercent: 0, ease: "none", scrollTrigger });
      });
      return () => mm.revert();
    },
    { scope: ref },
  );

  return (
    <div ref={ref} className={className} data-nav-tone="signal">
      {children}
    </div>
  );
}
