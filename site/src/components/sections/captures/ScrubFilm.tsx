"use client";

import { useRef } from "react";
import { useGSAP } from "@gsap/react";
import { gsap, MQ, setupGsap } from "@/lib/motion";
import { clipSrc, type Clip, type Sequence } from "@/data/media";
import { pickTier } from "./plan";
import { createPlayer } from "./sequence";

/** Viewport heights of scroll the film takes to play through, on top of the one it is pinned in. */
const TRAVEL = 0.9;

type Connection = { saveData?: boolean; effectiveType?: string };

/**
 * A film that plays with the scroll instead of the clock: the amp is whole at
 * the top of the chapter and has become a signal by the end of it. On a wide
 * screen it is a sequence of stills on a canvas, because a video cannot seek
 * as fast as a wheel turns. On a phone, where nothing is pinned, it is a
 * small video that loops.
 */
export default function ScrubFilm({
  sequence,
  loop,
  children,
}: {
  sequence: Sequence;
  loop: Clip;
  children: React.ReactNode;
}) {
  const scope = useRef<HTMLDivElement>(null);
  const loopSrc = clipSrc(loop);

  useGSAP(
    () => {
      setupGsap();
      const root = scope.current!;
      const canvas = root.querySelector("canvas")!;
      const video = root.querySelector("video")!;
      const mm = gsap.matchMedia();

      mm.add(`${MQ.motion} and ${MQ.desktop}`, () => {
        const connection = (navigator as Navigator & { connection?: Connection }).connection;
        const constrained = Boolean(connection?.saveData) || (connection?.effectiveType ?? "4g") !== "4g";
        const dpr = Math.min(window.devicePixelRatio, 2);
        const tier = pickTier(sequence, canvas.clientHeight * dpr, constrained);
        const player = createPlayer(canvas, sequence, tier.width);

        const ro = new ResizeObserver(() => {
          player.resize(Math.round(canvas.clientWidth * dpr), Math.round(canvas.clientHeight * dpr));
        });
        ro.observe(canvas);
        const io = new IntersectionObserver(
          ([entry]) => {
            if (!entry.isIntersecting) return;
            io.disconnect();
            player.load();
          },
          { rootMargin: "150% 0px" },
        );
        io.observe(root);

        const tl = gsap.timeline({
          defaults: { ease: "none" },
          scrollTrigger: {
            trigger: root,
            start: "top top",
            end: () => `+=${window.innerHeight * TRAVEL}`,
            pin: true,
            scrub: 0.5,
            onUpdate: (self) => player.seek(self.progress),
          },
        });
        tl.fromTo(canvas, { scale: 1.04 }, { scale: 1, duration: 1 }, 0);
        player.seek(0);

        return () => {
          ro.disconnect();
          io.disconnect();
          player.dispose();
        };
      });

      mm.add(`${MQ.motion} and not all and ${MQ.desktop}`, () => {
        const io = new IntersectionObserver(([entry]) => {
          if (entry.isIntersecting) video.play().catch(() => {});
          else video.pause();
        });
        io.observe(video);
        return () => io.disconnect();
      });

      return () => mm.revert();
    },
    { scope },
  );

  return (
    <div ref={scope} className="relative isolate flex flex-col overflow-hidden pt-20 md:h-svh md:flex-row md:items-center md:pt-0">
      <div
        aria-hidden
        className="relative -z-10 aspect-video w-full overflow-hidden md:absolute md:inset-y-0 md:right-0 md:aspect-auto md:w-[72%]"
      >
        {/* The first frame, in the page before any script runs; the canvas and the video draw over it. */}
        {/* eslint-disable-next-line @next/next/no-img-element */}
        <img src={sequence.poster} alt="" loading="lazy" decoding="async" className="absolute inset-0 size-full object-cover" />
        <canvas data-film className="absolute inset-0 hidden size-full md:motion-safe:block" />
        <video
          className="absolute inset-0 size-full object-cover md:hidden motion-reduce:hidden"
          muted
          loop
          playsInline
          preload="none"
          disablePictureInPicture
          tabIndex={-1}
        >
          <source src={loopSrc.mp4} type="video/mp4" />
        </video>
        <div className="absolute inset-y-0 left-0 hidden w-3/5 bg-linear-to-r from-ink via-ink/70 via-35% to-ink/0 md:block" />
        <div className="absolute inset-x-0 bottom-0 h-1/4 bg-linear-to-t from-ink to-ink/0" />
        <div className="absolute inset-x-0 top-0 h-1/4 bg-linear-to-b from-ink to-ink/0" />
      </div>
      <div className="w-full py-10 md:py-0 [&>div>*]:md:max-w-[min(34rem,40vw)]">{children}</div>
    </div>
  );
}
