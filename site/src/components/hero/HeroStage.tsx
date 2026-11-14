"use client";

import { useRef } from "react";
import { motion, useMotionValue, useScroll, useSpring, useTransform } from "motion/react";
import { useReducedMotion } from "@/components/SmoothScroll";
import VideoLoop from "@/components/ui/VideoLoop";
import Window from "@/components/ui/Window";
import { CLIPS } from "@/data/media";
import { EASE } from "@/lib/motion";

/** How the stack sits before anyone touches it, and how far the pointer can turn it. */
const REST = { x: 6, y: -14 };
const REACH = { x: 5, y: 8 };
const SPRING = { stiffness: 90, damping: 18, mass: 0.7 };

/**
 * Two layers of the real editor at two depths: the board being built, and in
 * front of it the same board stepping through presets. Neither clip plays
 * anywhere else on the page. The pointer turns the stack and the scroll lays
 * it flat, so the depth is something the visitor finds out.
 */
export default function HeroStage() {
  const root = useRef<HTMLDivElement>(null);
  const reduced = useReducedMotion();
  const mx = useMotionValue(0);
  const my = useMotionValue(0);

  const { scrollYProgress } = useScroll({ target: root, offset: ["start 20%", "end start"] });
  const flat = useTransform(scrollYProgress, [0, 0.85], [1, 0], { clamp: true });

  const rotateY = useSpring(
    useTransform([mx, flat], ([x, f]: number[]) => (REST.y + x * REACH.y) * f),
    SPRING,
  );
  const rotateX = useSpring(
    useTransform([my, flat], ([y, f]: number[]) => (REST.x - y * REACH.x) * f),
    SPRING,
  );
  const lift = useTransform(scrollYProgress, [0, 1], [0, -70]);
  const near = useTransform(flat, [0, 1], [24, 130]);

  const onMove = (e: React.PointerEvent<HTMLDivElement>) => {
    const r = e.currentTarget.getBoundingClientRect();
    mx.set(((e.clientX - r.left) / r.width - 0.5) * 2);
    my.set(((e.clientY - r.top) / r.height - 0.5) * 2);
  };
  const rest = () => {
    mx.set(0);
    my.set(0);
  };

  if (reduced) {
    return (
      <div ref={root}>
        <Window>
          <VideoLoop clip={CLIPS.chainBuild} play={false} className="block h-auto w-full" />
        </Window>
      </div>
    );
  }

  return (
    <div
      ref={root}
      onPointerMove={onMove}
      onPointerLeave={rest}
      className="relative [perspective:1600px] lg:[perspective-origin:30%_50%]"
    >
      <motion.div
        initial={{ opacity: 0, rotateY: -38, rotateX: 14, z: -260 }}
        animate={{ opacity: 1, rotateY: 0, rotateX: 0, z: 0 }}
        transition={{ duration: 1.5, ease: EASE.out, delay: 0.15 }}
        style={{ transformStyle: "preserve-3d" }}
      >
        <motion.div style={{ rotateX, rotateY, y: lift, transformStyle: "preserve-3d" }} className="relative origin-center">
          <Window className="shadow-[0_40px_120px_-20px_rgb(0_0_0/0.8)]">
            <VideoLoop clip={CLIPS.chainBuild} className="block h-auto w-full" />
          </Window>

          <motion.div style={{ z: near }} className="absolute -bottom-[16%] -left-[3%] w-[56%] lg:-left-[10%]">
            <Window className="shadow-[0_30px_80px_-10px_rgb(0_0_0/0.85)]">
              <VideoLoop clip={CLIPS.presets} className="block h-auto w-full" />
            </Window>
          </motion.div>
        </motion.div>
      </motion.div>
    </div>
  );
}
