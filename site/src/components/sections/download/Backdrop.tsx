"use client";

import { useRef } from "react";
import { motion, useScroll, useTransform } from "motion/react";
import { useReducedMotion } from "@/components/SmoothScroll";

/** Atmosphere behind the download card. It moves slower than the page, so the card reads as nearer. */
export default function Backdrop({ children }: { children: React.ReactNode }) {
  const ref = useRef<HTMLDivElement>(null);
  const reduced = useReducedMotion();
  const { scrollYProgress } = useScroll({ target: ref, offset: ["start end", "end start"] });
  const y = useTransform(scrollYProgress, [0, 1], ["-12%", "12%"]);

  return (
    <div ref={ref} aria-hidden className="absolute inset-x-0 top-0 -z-10 h-[min(100svh,56rem)] overflow-hidden">
      <motion.div style={reduced ? undefined : { y }} className="absolute inset-0 scale-125 opacity-45">
        {children}
      </motion.div>
      <div className="absolute inset-0 bg-linear-to-b from-ink via-ink/30 to-ink" />
    </div>
  );
}
