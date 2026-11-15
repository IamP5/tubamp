"use client";

import { useRef } from "react";
import { useGSAP } from "@gsap/react";
import { gsap, MQ, setupGsap } from "@/lib/motion";
import { CLIPS } from "@/data/media";
import { useDict } from "@/i18n/DictProvider";
import VideoLoop from "@/components/ui/VideoLoop";
import Tilt from "@/components/ui/Tilt";
import Window from "@/components/ui/Window";
import ArriveClip from "./ArriveClip";

/** Scroll travel of each layer, in px, across the stage's time on screen. The nearer layer travels further. */
const DEPTH = { back: [40, -40], front: [160, -140] } as const;

export default function CapturesStage() {
  const scope = useRef<HTMLDivElement>(null);
  const t = useDict();

  useGSAP(
    () => {
      setupGsap();
      const mm = gsap.matchMedia();
      mm.add(`${MQ.motion} and ${MQ.desktop}`, () => {
        const scrollTrigger = { trigger: scope.current, start: "top bottom", end: "bottom top", scrub: true };
        for (const layer of ["back", "front"] as const) {
          const [from, to] = DEPTH[layer];
          gsap.fromTo(`[data-depth="${layer}"]`, { y: from }, { y: to, ease: "none", scrollTrigger });
        }
      });
      return () => mm.revert();
    },
    { scope },
  );

  return (
    <div ref={scope} className="relative">
      <div data-depth="back" className="md:w-[80%]">
        <ArriveClip clip={CLIPS.amp} from="center" tilt />
      </div>
      <figure data-depth="front" className="relative mt-6 md:ml-auto md:-mt-[36%] md:w-[30%]">
        <Tilt>
          <Window className="shadow-[0_0_0_10px_var(--color-ink)]">
            {/* The browser is the panel on the right of the plugin window; on wide screens show only that panel. */}
            <VideoLoop
              clip={CLIPS.t3k}
              className="block h-auto w-full md:aspect-[1/2]! md:object-cover md:object-right"
            />
          </Window>
        </Tilt>
        <figcaption className="mt-4 flex items-center gap-3 text-sm text-bone-2">
          <span className="h-px w-6 bg-[var(--accent)]" aria-hidden />
          {t.captures.attribution}
        </figcaption>
      </figure>
    </div>
  );
}
