"use client";

import { useRef } from "react";
import { useGSAP } from "@gsap/react";
import { gsap, MQ, setupGsap } from "@/lib/motion";
import VideoLoop from "@/components/ui/VideoLoop";
import Tilt from "@/components/ui/Tilt";
import Window from "@/components/ui/Window";
import type { Clip } from "@/data/media";

type From = "center" | "left" | "right";

/** Where the window's clip starts. The radius matches Window's rounded-xl. */
const CLOSED: Record<From, string> = {
  center: "inset(16% 12% 16% 12% round 12px)",
  left: "inset(0% 100% 0% 0% round 12px)",
  right: "inset(0% 0% 0% 100% round 12px)",
};
// Open past the frame so a tilted window keeps its corners.
const OPEN = "inset(-4% -4% -4% -4% round 12px)";

export default function ArriveClip({
  clip,
  from = "center",
  className = "",
  tilt = false,
}: {
  clip: Clip;
  from?: From;
  className?: string;
  tilt?: boolean;
}) {
  const scope = useRef<HTMLDivElement>(null);

  useGSAP(
    () => {
      setupGsap();
      const mm = gsap.matchMedia();
      mm.add(MQ.motion, () => {
        const trigger = { trigger: scope.current, start: "top 92%", end: "top 40%", scrub: 0.6 };
        gsap.fromTo(
          scope.current,
          { clipPath: CLOSED[from] },
          { clipPath: OPEN, ease: "none", scrollTrigger: trigger },
        );
        gsap.fromTo("video", { scale: 1.14 }, { scale: 1, ease: "none", scrollTrigger: trigger });
      });
      return () => mm.revert();
    },
    { scope },
  );

  const framed = (
    <Window>
      <VideoLoop clip={clip} className="block h-auto w-full" />
    </Window>
  );

  return (
    <div ref={scope} className={className}>
      {tilt ? <Tilt>{framed}</Tilt> : framed}
    </div>
  );
}
