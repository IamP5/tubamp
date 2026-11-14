"use client";

import { useEffect, useSyncExternalStore } from "react";
import { ReactLenis, useLenis } from "lenis/react";
import { gsap, MQ, ScrollTrigger, setupGsap } from "@/lib/motion";

const subscribeReduced = (cb: () => void) => {
  const mq = window.matchMedia(MQ.reduced);
  mq.addEventListener("change", cb);
  return () => mq.removeEventListener("change", cb);
};

export function useReducedMotion(): boolean {
  return useSyncExternalStore(
    subscribeReduced,
    () => window.matchMedia(MQ.reduced).matches,
    () => false,
  );
}

/**
 * ReactLenis creates its instance in an effect of its own, so the hookup lives
 * in a child that re-runs once `useLenis` has the instance.
 */
function TickerBridge() {
  const lenis = useLenis();

  useEffect(() => {
    if (!lenis) return;
    const tick = (time: number) => lenis.raf(time * 1000);
    lenis.on("scroll", ScrollTrigger.update);
    gsap.ticker.add(tick);
    gsap.ticker.lagSmoothing(0);
    return () => {
      lenis.off("scroll", ScrollTrigger.update);
      gsap.ticker.remove(tick);
    };
  }, [lenis]);

  return null;
}

export default function SmoothScroll({ children }: { children: React.ReactNode }) {
  const reduced = useReducedMotion();

  useEffect(() => {
    setupGsap();
  }, []);

  if (reduced) return <>{children}</>;
  return (
    <ReactLenis root options={{ lerp: 0.1, autoRaf: false, anchors: true }}>
      <TickerBridge />
      {children}
    </ReactLenis>
  );
}
