"use client";

import { useRef } from "react";
import { useGSAP } from "@gsap/react";
import { CREDITS, SITE } from "@/data/site";
import { gsap, MQ, setupGsap } from "@/lib/motion";

export default function Footer() {
  const ref = useRef<HTMLElement>(null);

  useGSAP(
    () => {
      setupGsap();
      const mm = gsap.matchMedia();
      mm.add(MQ.motion, () => {
        gsap.fromTo(
          "[data-letter]",
          { yPercent: 70 },
          {
            yPercent: 0,
            ease: "none",
            stagger: 0.08,
            scrollTrigger: { trigger: ref.current, start: "top bottom", end: "bottom bottom", scrub: 0.6 },
          },
        );
      });
      return () => mm.revert();
    },
    { scope: ref },
  );

  return (
    <footer ref={ref} className="relative overflow-hidden bg-ink text-bone">
      <div className="gutter flex flex-col gap-6 pt-14 text-sm md:flex-row md:items-baseline md:justify-between md:gap-10">
        <a
          href={SITE.repoUrl}
          className="shrink-0 text-bone underline decoration-line-strong underline-offset-4 transition-colors duration-150 ease-ui hover:decoration-bone"
        >
          Source on GitHub
        </a>
        <ul className="flex flex-wrap gap-x-6 gap-y-2 text-bone-2 md:justify-end">
          {CREDITS.map((credit) => (
            <li key={credit}>{credit}</li>
          ))}
        </ul>
      </div>
      <p className="display mt-[clamp(3rem,8vw,7rem)] flex justify-center overflow-hidden text-[27.5vw] leading-[0.76] select-none">
        <span className="sr-only">{SITE.name}</span>
        {Array.from(SITE.name).map((letter, i) => (
          <span key={i} data-letter aria-hidden className="inline-block will-change-transform">
            {letter}
          </span>
        ))}
      </p>
    </footer>
  );
}
