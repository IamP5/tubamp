"use client";

import { useSyncExternalStore, type PointerEvent, type ReactNode } from "react";
import { motion, useMotionValue, useSpring, useTransform } from "motion/react";
import { useReducedMotion } from "@/components/SmoothScroll";
import { MQ, ScrollTrigger } from "@/lib/motion";

const MAX_DEG = 3.5;
const SPRING = { stiffness: 160, damping: 22, mass: 0.6 };

const subscribePointer = (cb: () => void) => {
  const mq = window.matchMedia(MQ.pointer);
  mq.addEventListener("change", cb);
  return () => mq.removeEventListener("change", cb);
};

function useFinePointer() {
  return useSyncExternalStore(subscribePointer, () => window.matchMedia(MQ.pointer).matches, () => false);
}

/** A pinned, scrubbed stage already moves with the scroll; tilting it on top would fight the scrub. */
function insideActivePin(el: Element) {
  return ScrollTrigger.getAll().some((st) => st.isActive && st.pin instanceof Element && st.pin.contains(el));
}

export default function Tilt({ children, className = "" }: { children: ReactNode; className?: string }) {
  const reduced = useReducedMotion();
  const fine = useFinePointer();
  const mx = useMotionValue(0.5);
  const my = useMotionValue(0.5);
  const rotateY = useSpring(useTransform(mx, [0, 1], [-MAX_DEG, MAX_DEG]), SPRING);
  const rotateX = useSpring(useTransform(my, [0, 1], [MAX_DEG, -MAX_DEG]), SPRING);
  const active = fine && !reduced;

  const rest = () => {
    mx.set(0.5);
    my.set(0.5);
  };
  const onMove = (e: PointerEvent<HTMLDivElement>) => {
    if (insideActivePin(e.currentTarget)) return rest();
    const r = e.currentTarget.getBoundingClientRect();
    mx.set((e.clientX - r.left) / r.width);
    my.set((e.clientY - r.top) / r.height);
  };

  return (
    <div
      className={className}
      style={{ perspective: 1400 }}
      onPointerMove={active ? onMove : undefined}
      onPointerLeave={active ? rest : undefined}
    >
      <motion.div style={{ rotateX, rotateY, transformStyle: "preserve-3d" }}>{children}</motion.div>
    </div>
  );
}
