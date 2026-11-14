"use client";

import { Fragment, useRef, type ReactNode } from "react";
import { useGSAP } from "@gsap/react";
import { useReducedMotion } from "@/components/SmoothScroll";
import VideoLoop from "@/components/ui/VideoLoop";
import type { Clip } from "@/data/media";
import { gsap, MQ, ScrollTrigger, setupGsap } from "@/lib/motion";

export type Opening = "blinds" | "grid";

/** Fractions of the frame. It collapses toward `origin` when its turn comes (`at`, 0..1). */
type Shutter = { x: number; y: number; w: number; h: number; at: number; origin: string };

// Shutters overlap by this much so no hairline of footage shows while the frame is closed.
const SEAM = 0.004;

function blinds(count: number): Shutter[] {
  const mid = (count - 1) / 2;
  return Array.from({ length: count }, (_, i) => {
    const at = Math.abs(i - mid) / Math.max(mid, 1);
    const half = 1 / count / 2;
    return [
      { x: 0, y: i / count, w: 1, h: half, at, origin: "50% 0%" },
      { x: 0, y: i / count + half, w: 1, h: half, at, origin: "50% 100%" },
    ];
  }).flat();
}

function grid(cols: number, aspect: number): Shutter[] {
  const rows = Math.max(2, Math.round(cols / aspect));
  const cells: Shutter[] = [];
  for (let c = 0; c < cols; c++) {
    const order = gsap.utils.shuffle(Array.from({ length: rows }, (_, r) => r));
    order.forEach((r, rank) => {
      cells.push({ x: c / cols, y: r / rows, w: 1 / cols, h: 1 / rows, at: (c + (rank / rows) * 2.5) / (cols + 2.5), origin: "50% 50%" });
    });
  }
  return cells;
}

function shuttersFor(opening: Opening, width: number, height: number): Shutter[] {
  if (width < 768) return blinds(8);
  return opening === "blinds" ? blinds(16) : grid(width < 1100 ? 10 : 14, width / height);
}

/**
 * Whatever is passed as `children` rises over the held frame.
 * Under reduced motion it is the poster and the line, laid out in place.
 */
export default function Interlude({
  film,
  line,
  turn,
  opening,
  children,
}: {
  film: Clip;
  line: string;
  /** The end of `line` that takes the serif. */
  turn: string;
  opening: Opening;
  children?: ReactNode;
}) {
  const root = useRef<HTMLDivElement>(null);
  const reduced = useReducedMotion();
  const cut = line.lastIndexOf(turn);
  const lead = line.slice(0, cut).trim().split(" ");
  const accent = line.slice(cut).split(" ");

  useGSAP(
    () => {
      setupGsap();
      const el = root.current!;
      const q = gsap.utils.selector(el);
      const frame = q("[data-frame]")[0] as HTMLElement;
      const group = q("[data-shutters]")[0] as HTMLElement;
      const mm = gsap.matchMedia();

      mm.add(MQ.motion, () => {
        const narrow = !window.matchMedia(MQ.desktop).matches;
        let open: gsap.core.Timeline | null = null;

        const build = () => {
          open?.scrollTrigger?.kill();
          open?.kill();
          group.replaceChildren();
          const shutters = shuttersFor(opening, frame.offsetWidth, frame.offsetHeight);
          open = gsap.timeline({
            defaults: { ease: "none" },
            scrollTrigger: { trigger: el, start: "top bottom", end: "top top", scrub: 1 },
          });
          for (const cell of shutters) {
            const div = document.createElement("div");
            div.className = "absolute bg-ink";
            Object.assign(div.style, {
              left: `${cell.x * 100}%`,
              top: `${cell.y * 100}%`,
              width: `${(cell.w + SEAM) * 100}%`,
              height: `${(cell.h + SEAM) * 100}%`,
              transformOrigin: cell.origin,
            });
            group.appendChild(div);
            const to = cell.w === 1 ? { scaleY: 0 } : { scale: 0 };
            open.to(div, { ...to, duration: 0.3, ease: "power2.in" }, 0.05 + cell.at * 0.45);
          }
          open.from(q("[data-word]"), { yPercent: 115, duration: 0.25, stagger: 0.05, ease: "power3.out" }, 0.5);
          open.to({}, { duration: 0.05 });
        };

        build();

        let width = window.innerWidth;
        let height = window.innerHeight;
        const onResize = () => {
          // A phone's toolbar changes the height while scrolling; rebuild only for a real resize.
          if (window.innerWidth === width && Math.abs(window.innerHeight - height) < 120) return;
          width = window.innerWidth;
          height = window.innerHeight;
          build();
          ScrollTrigger.refresh();
        };
        const resized = gsap.delayedCall(0.2, onResize).pause();
        const queue = () => resized.restart(true);
        window.addEventListener("resize", queue);

        const drift = narrow ? 3 : 6;
        gsap.fromTo(
          q("[data-drift]"),
          { yPercent: -drift },
          { yPercent: drift, ease: "none", scrollTrigger: { trigger: el, start: "top bottom", end: "bottom top", scrub: true } },
        );

        const over = q("[data-over]")[0];
        if (over) {
          const sink = { trigger: over, start: "top bottom", end: "top top", scrub: true };
          gsap.fromTo(q("[data-sink]"), { yPercent: 0 }, { yPercent: -12, ease: "none", scrollTrigger: sink });
          gsap.fromTo(q("[data-veil]"), { opacity: 0 }, { opacity: 0.75, ease: "none", scrollTrigger: sink });
        }

        return () => {
          window.removeEventListener("resize", queue);
          resized.kill();
          open?.scrollTrigger?.kill();
          open?.kill();
          group.replaceChildren();
        };
      });

      return () => mm.revert();
    },
    { scope: root, dependencies: [opening] },
  );

  const words = [...lead.map((w) => ({ w, accent: false })), ...accent.map((w) => ({ w, accent: true }))];

  return (
    <div ref={root} className="relative bg-ink">
      <div data-frame className="relative h-svh overflow-hidden bg-ink motion-safe:sticky motion-safe:top-0">
        <div data-sink className="absolute inset-0">
          <div aria-hidden className="absolute inset-0">
            <div data-drift className="absolute inset-x-0 -inset-y-[8%]">
              <VideoLoop clip={film} play={!reduced} className="absolute inset-0 size-full object-cover" />
            </div>
          </div>
          <div data-shutters aria-hidden className="absolute inset-0" />
          <div aria-hidden className="absolute inset-0 bg-ink/35" />
          <div aria-hidden className="absolute inset-x-0 top-0 h-[32%] bg-linear-to-b from-ink to-ink/0" />
          <div aria-hidden className="absolute inset-x-0 bottom-0 h-[45%] bg-linear-to-t from-ink via-ink/70 to-ink/0" />
          <p className="gutter display absolute inset-x-0 bottom-[12svh] max-w-[15ch] text-balance text-display text-bone md:bottom-[14svh] md:max-w-[13ch] md:box-content">
            {words.map(({ w, accent: serif }, i) => (
              <Fragment key={i}>
                {i > 0 && " "}
                <span className="-my-[0.14em] inline-block overflow-clip py-[0.14em] align-bottom">
                  <span data-word className={`inline-block ${serif ? "serif-accent pr-[0.06em]" : ""}`}>
                    {w}
                  </span>
                </span>
              </Fragment>
            ))}
          </p>
        </div>
        <div data-veil aria-hidden className="pointer-events-none absolute inset-0 bg-ink opacity-0" />
      </div>
      <div aria-hidden className="hidden h-[45svh] motion-safe:block" />
      {children && (
        <div data-over className="relative z-10">
          {children}
        </div>
      )}
    </div>
  );
}
