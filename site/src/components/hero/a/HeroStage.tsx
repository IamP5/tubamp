"use client";

import { useRef, type ReactNode } from "react";
import { useGSAP } from "@gsap/react";
import { gsap, MQ, ScrollTrigger, setupGsap } from "@/lib/motion";
import { createMask } from "./mask";

const FULL = `${MQ.motion} and ${MQ.desktop}`;

/** `glow` is how far down the frame the row of filaments burns (0 = top, 1 = bottom). */
const FILM = { src: "/media/film/tubes", width: 1920, height: 1080, glow: 0.53 };

/**
 * Where to put the footage so its row of filaments lands at `target` (a y in
 * the window, in px) when the frame is scaled by at least `minScale`: an
 * offset, and the scale that keeps the shifted frame covering the window.
 */
function filmFit(video: HTMLVideoElement, target: number, minScale = 1) {
  const w = video.offsetWidth;
  const h = video.offsetHeight;
  const k = Math.max(w / FILM.width, h / FILM.height);
  const glow = (h - FILM.height * k) / 2 + FILM.glow * FILM.height * k;
  const fit = (scale: number) => ({ scale, y: target - h / 2 - (glow - h / 2) * scale });
  let f = fit(minScale);
  while (f.scale < 1 + (2 * Math.abs(f.y)) / h) f = fit(f.scale + 0.02);
  return f;
}

/** A point `at` of the way down the wordmark, in the coordinates of the film's window. */
function wordmarkY(video: HTMLVideoElement, wordmark: HTMLElement, at = 0.5) {
  const word = wordmark.getBoundingClientRect();
  return word.top + word.height * at - video.parentElement!.getBoundingClientRect().top;
}

/**
 * The hero's moving part. On desktop with motion allowed the stage pins for one
 * and a half screens while the camera flies into the wordmark and out the other
 * side into the footage, where the headline lands. Everywhere else the
 * wordmark is a still window onto the footage with the copy below it.
 *
 * `md:motion-safe:` classes belong to the pinned layout and `md:motion-reduce:`
 * to the still desktop one, so the server render already matches whichever
 * version the visitor will get.
 */
export default function HeroStage({
  headline,
  description,
  line,
  actions,
}: {
  headline: ReactNode;
  description: ReactNode;
  line: ReactNode;
  actions: ReactNode;
}) {
  const root = useRef<HTMLDivElement>(null);

  useGSAP(
    () => {
      setupGsap();
      const stage = root.current!.querySelector<HTMLElement>("[data-stage]")!;
      const q = gsap.utils.selector(stage);
      const canvas = q("canvas")[0] as HTMLCanvasElement;
      const video = q("video")[0] as HTMLVideoElement;
      const wordmark = q("[data-wordmark]")[0] as HTMLElement;
      const colours = {
        ink: getComputedStyle(stage).backgroundColor,
        edge: getComputedStyle(stage).getPropertyValue("--color-line-strong").trim(),
      };

      const playWhileVisible = (target: Element) => {
        const io = new IntersectionObserver(([entry]) => {
          if (entry.isIntersecting) video.play().catch(() => {});
          else video.pause();
        });
        io.observe(target);
        const warm = () => (video.dataset.on = "");
        video.addEventListener("playing", warm, { once: true });
        return () => {
          io.disconnect();
          video.removeEventListener("playing", warm);
        };
      };

      /** Paints the mask once fonts are in, and again whenever the stage changes size. */
      const startMask = (zoom: boolean, onMeasure: (paint: (t: number) => void) => void) => {
        const mask = createMask(canvas, wordmark, colours, zoom);
        let live = true;
        const ro = new ResizeObserver(() => {
          mask.measure();
          onMeasure(mask.paint);
        });
        document.fonts.ready.then(() => {
          if (!live) return;
          ro.observe(canvas);
          stage.dataset.ready = "";
        });
        return () => {
          live = false;
          ro.disconnect();
          delete stage.dataset.ready;
          canvas.style.visibility = "";
        };
      };

      const mm = gsap.matchMedia();

      mm.add(FULL, () => {
        const zoom = { t: 0 };
        let paint: (t: number) => void = () => {};
        // The filaments run through the letters at the start; once the camera is
        // through, they sit in the upper half, above the headline.
        let from = { y: 0, scale: 1 };
        let to = { y: 0, scale: 1 };
        const stopMask = startMask(true, (p) => {
          paint = p;
          from = filmFit(video, wordmarkY(video, wordmark));
          to = filmFit(video, video.offsetHeight * 0.36, from.scale * 1.3);
          ScrollTrigger.refresh();
        });

        gsap.set(q("[data-copy]"), { opacity: 1 });
        const tl = gsap.timeline({
          defaults: { ease: "none" },
          scrollTrigger: {
            trigger: stage,
            start: "top top",
            end: "+=150%",
            pin: true,
            scrub: true,
            invalidateOnRefresh: true,
          },
        });
        tl.to(q("[data-line]"), { autoAlpha: 0, y: -32, duration: 0.12, ease: "power2.in" }, 0)
          .to(zoom, { t: 1, duration: 0.62, onUpdate: () => paint(zoom.t) }, 0)
          .fromTo(
            video,
            { y: () => from.y, scale: () => from.scale },
            { y: () => to.y, scale: () => to.scale, duration: 1 },
            0,
          )
          .to(q("[data-scrim]"), { opacity: 1, duration: 0.25 }, 0.5)
          .from(q("[data-word]"), { yPercent: 130, duration: 0.14, stagger: 0.025 }, 0.6)
          .from(q("[data-description]"), { autoAlpha: 0, y: 24, duration: 0.14 }, 0.72);

        const stopFilm = playWhileVisible(stage);
        return () => {
          stopMask();
          stopFilm();
        };
      });

      mm.add(`not all and ${FULL}`, () => {
        // Stacked on narrow screens: a closer frame widens the glow, aimed
        // between the rows so both catch the lit glass.
        const stacked = !window.matchMedia(MQ.desktop).matches;
        const stopMask = startMask(false, () =>
          gsap.set(video, filmFit(video, wordmarkY(video, wordmark, stacked ? 0.45 : 0.5), stacked ? 1.8 : 1)),
        );
        if (!window.matchMedia(MQ.reduced).matches) {
          const stopFilm = playWhileVisible(video);
          return () => {
            stopMask();
            stopFilm();
          };
        }
        video.pause();
        video.dataset.on = "";
        return stopMask;
      });
    },
    { scope: root },
  );

  return (
    <div ref={root}>
      <div
        data-stage
        className="group relative isolate overflow-hidden bg-ink md:motion-safe:flex md:motion-safe:h-svh md:motion-safe:flex-col"
      >
        <div className="gutter @container relative pt-24 pb-4 md:motion-safe:static md:motion-safe:mt-auto md:motion-safe:py-0">
          <div aria-hidden className="absolute inset-0 -z-10 overflow-hidden bg-ink group-data-ready:bg-ink-3">
            <video
              className="absolute inset-0 size-full object-cover opacity-0 transition-opacity duration-[1600ms] ease-out data-on:opacity-100"
              poster={`${FILM.src}.jpg`}
              muted
              loop
              playsInline
              preload="none"
              disablePictureInPicture
              tabIndex={-1}
            >
              <source src={`${FILM.src}.webm`} type="video/webm" />
              <source src={`${FILM.src}.mp4`} type="video/mp4" />
            </video>
            <canvas className="absolute inset-0 size-full" />
            <div
              data-scrim
              className="absolute inset-0 bg-linear-to-t from-ink from-10% via-ink/70 to-ink/0 opacity-0"
            />
          </div>
          <p
            aria-hidden
            data-wordmark
            className="display text-[length:calc(100cqw/1.94)] leading-[0.8] text-ink-3 md:leading-[0.88] select-none group-data-ready:text-transparent md:text-[length:min(calc(100cqw/3.44),52svh)]"
          >
            tub
            <br className="md:hidden" />
            amp
          </p>
        </div>

        <div className="gutter relative pt-10 pb-12 md:motion-safe:pt-[9svh] md:motion-safe:pb-10 md:motion-reduce:grid md:motion-reduce:grid-cols-[auto_minmax(0,30rem)] md:motion-reduce:grid-rows-[1fr_auto] md:motion-reduce:justify-between md:motion-reduce:gap-x-12">
          <div className="relative md:motion-reduce:contents">
            <div data-line aria-hidden className="hidden md:motion-safe:block">
              {line}
            </div>
            <div
              data-copy
              className="md:motion-safe:absolute md:motion-safe:inset-x-0 md:motion-safe:bottom-0 md:motion-safe:opacity-0 md:motion-reduce:contents"
            >
              <div className="md:motion-reduce:row-span-2 md:motion-reduce:self-end">{headline}</div>
              <div data-description className="mt-6 md:mt-8 md:motion-reduce:mt-0 md:motion-reduce:self-end">
                {description}
              </div>
            </div>
          </div>
          <div className="mt-8 md:motion-reduce:col-start-2 md:motion-reduce:mt-6">{actions}</div>
        </div>
      </div>
    </div>
  );
}
