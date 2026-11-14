import gsap from "gsap";
import { ScrollTrigger } from "gsap/ScrollTrigger";

let registered = false;

export function setupGsap() {
  if (registered || typeof window === "undefined") return;
  registered = true;
  gsap.registerPlugin(ScrollTrigger);
  gsap.defaults({ ease: "expo.out", duration: 0.9 });
}

export const MQ = {
  motion: "(prefers-reduced-motion: no-preference)",
  reduced: "(prefers-reduced-motion: reduce)",
  desktop: "(min-width: 768px)",
  pointer: "(hover: hover) and (pointer: fine)",
} as const;

export const EASE = {
  out: [0.16, 1, 0.3, 1],
  ui: [0.2, 0, 0, 1],
  inOut: [0.65, 0, 0.35, 1],
} as const satisfies Record<string, readonly [number, number, number, number]>;

export { gsap, ScrollTrigger };
