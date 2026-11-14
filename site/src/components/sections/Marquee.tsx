"use client";

import { useRef } from "react";
import { useGSAP } from "@gsap/react";
import { gsap, MQ, ScrollTrigger, setupGsap } from "@/lib/motion";
import { CHAIN } from "@/data/site";

const ITEMS = [...CHAIN.map((b) => b.name), "Your own plugins"];

function Row() {
  return (
    <ul className="flex shrink-0 items-center">
      {ITEMS.map((name) => (
        <li key={name} className="flex items-center gap-[0.45em] pr-[0.45em] whitespace-nowrap">
          {name}
          <span className="inline-block size-[0.16em] rounded-full bg-current" />
        </li>
      ))}
    </ul>
  );
}

export default function Marquee() {
  const scope = useRef<HTMLDivElement>(null);

  useGSAP(
    () => {
      setupGsap();
      const mm = gsap.matchMedia();
      mm.add({ motion: MQ.motion, pointer: MQ.pointer }, (ctx) => {
        const { motion, pointer } = ctx.conditions as { motion: boolean; pointer: boolean };
        if (!motion) return;
        const loop = gsap.to("[data-track]", { xPercent: -50, ease: "none", duration: 34, repeat: -1 });
        const cap = pointer ? 5 : 3;
        let dir = 1;
        ScrollTrigger.create({
          trigger: scope.current,
          start: "top bottom",
          end: "bottom top",
          onUpdate: (self) => {
            dir = self.direction;
            const boost = 1 + Math.min(Math.abs(self.getVelocity()) / 600, cap);
            gsap.to(loop, { timeScale: dir * boost, duration: 0.25, overwrite: true });
            gsap.to(loop, { timeScale: dir, duration: 1.2, delay: 0.25, ease: "power2.out" });
          },
        });
      });
      return () => mm.revert();
    },
    { scope },
  );

  return (
    <div ref={scope} className="relative overflow-x-clip py-[6svh]">
      <div className="relative left-1/2 w-[112vw] -translate-x-1/2 -rotate-2 overflow-hidden bg-signal py-[0.32em] text-on-signal">
        <p className="sr-only">{ITEMS.join(", ")}</p>
        <div
          data-track
          aria-hidden
          className="display flex w-max text-[clamp(2.75rem,7vw,7.5rem)] leading-[1.02] will-change-transform"
        >
          <Row />
          <Row />
        </div>
      </div>
    </div>
  );
}
