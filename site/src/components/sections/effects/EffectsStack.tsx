"use client";

import { Fragment, useRef, useState } from "react";
import { useGSAP } from "@gsap/react";
import { useLenis } from "lenis/react";
import { gsap, MQ, ScrollTrigger, setupGsap } from "@/lib/motion";
import VideoLoop from "@/components/ui/VideoLoop";
import Window from "@/components/ui/Window";
import { EFFECT_PANELS } from "./panels";

/** Pinned rest after each panel has risen, before the next one starts to cover it. */
const HOLD_SVH = 30;
const pad = (n: number) => String(n).padStart(2, "0");
const TOTAL = pad(EFFECT_PANELS.length);

/**
 * Eight editor strips as a stack of full-screen panels. Each panel is a
 * sticky sibling of the others, so a later panel rises over an earlier one
 * that stays pinned; the covered panel sinks and darkens as it goes under.
 * Every panel re-points `--accent` to its block's family, so the giant name
 * behind the strip and the index below change hue with the block.
 *
 * Narrow screens and reduced motion get the same markup unpinned: a plain
 * vertical sequence of name, line and strip.
 */
export default function EffectsStack() {
  const scope = useRef<HTMLDivElement>(null);
  const marks = useRef<(HTMLDivElement | null)[]>([]);
  const [active, setActive] = useState(0);
  const lenis = useLenis();

  useGSAP(
    () => {
      setupGsap();
      const mm = gsap.matchMedia();
      mm.add(`${MQ.motion} and ${MQ.desktop}`, () => {
        const panels = gsap.utils.toArray<HTMLElement>("[data-panel]", scope.current);
        panels.forEach((panel, i) => {
          const mark = marks.current[i];
          const rise = { trigger: mark, start: "top bottom", end: "top top", scrub: true };
          gsap.fromTo(panel.querySelector("[data-name]"), { yPercent: 38 }, { yPercent: 0, ease: "none", scrollTrigger: rise });
          gsap.fromTo(panel.querySelector("[data-strip]"), { y: 110 }, { y: 0, ease: "none", scrollTrigger: rise });

          const under = panels[i - 1];
          if (under) {
            gsap.fromTo(
              under.querySelector("[data-inner]"),
              { scale: 1 },
              { scale: 0.93, transformOrigin: "50% 40%", ease: "none", scrollTrigger: rise },
            );
            gsap.fromTo(under.querySelector("[data-veil]"), { opacity: 0 }, { opacity: 0.7, ease: "none", scrollTrigger: rise });
          }

          ScrollTrigger.create({
            trigger: mark,
            start: "top 50%",
            onEnter: () => setActive(i),
            onLeaveBack: () => setActive(Math.max(0, i - 1)),
          });
        });

        // The index belongs to the stack: it leaves with the last panel instead of riding up into the next chapter.
        gsap.to("[data-index]", {
          autoAlpha: 0,
          ease: "none",
          scrollTrigger: { trigger: scope.current, start: "bottom bottom", end: "bottom 85%", scrub: true },
        });
      });
      return () => mm.revert();
    },
    { scope },
  );

  const jump = (i: number) => {
    const mark = marks.current[i];
    if (!mark) return;
    const top = mark.getBoundingClientRect().top + window.scrollY + 1;
    if (lenis) lenis.scrollTo(top, { duration: 1.2 });
    else window.scrollTo({ top });
  };

  return (
    <div ref={scope} className="relative">
      {EFFECT_PANELS.map(({ block, clip }, i) => (
        <Fragment key={block.id}>
          <div
            ref={(el) => {
              marks.current[i] = el;
            }}
            aria-hidden
            className="h-0"
          />
          <article
            data-panel
            data-family={block.family}
            aria-labelledby={`effect-${block.id}`}
            style={{ zIndex: i + 1 }}
            className="relative bg-ink md:motion-safe:sticky md:motion-safe:top-0 md:motion-safe:h-svh md:motion-safe:overflow-hidden"
          >
            <div
              data-inner
              className="gutter mx-auto flex max-w-[100rem] flex-col border-t border-line py-16 md:border-t-0 md:py-24 md:motion-safe:h-full md:motion-safe:justify-center md:motion-safe:pt-[10svh] md:motion-safe:pb-[16svh]"
            >
              <div className="flex flex-wrap items-baseline gap-x-8 gap-y-2">
                <p className="label">
                  <span className="text-[var(--accent)]">{pad(i + 1)}</span> / {TOTAL}
                </p>
                {block.types && (
                  <ul className="flex flex-wrap gap-x-4 text-sm text-bone-2" aria-label={`${block.name} types`}>
                    {block.types.map((type) => (
                      <li key={type}>{type}</li>
                    ))}
                  </ul>
                )}
              </div>
              <h3
                id={`effect-${block.id}`}
                data-name
                className="display mt-4 text-h2 text-bone md:mt-6 md:text-[min(15.5vw,28svh)] md:leading-[0.8] md:text-[var(--accent)]"
              >
                {block.name}
              </h3>
              <p className="mt-4 text-lg text-bone-2 md:hidden">{block.does}</p>
              <div data-strip className="relative mt-6 md:-mt-[min(2.8vw,5svh)] md:max-w-[min(100%,150svh)]">
                <Window className="shadow-[0_0_0_8px_var(--color-ink)]">
                  {/* On a phone the full strip is too small to read; show its left half, from the header in. */}
                  <VideoLoop clip={clip} className="block h-auto w-full max-md:aspect-[2/1]! max-md:object-cover max-md:object-left" />
                </Window>
              </div>
              <p className="mt-8 hidden text-h3 leading-tight text-bone md:block">{block.does}</p>
            </div>
            <div data-veil aria-hidden className="pointer-events-none absolute inset-0 bg-ink opacity-0" />
          </article>
          <div aria-hidden className="hidden md:motion-safe:block" style={{ height: `${HOLD_SVH}svh` }} />
        </Fragment>
      ))}

      <nav
        aria-label="Effect editors"
        className="pointer-events-none absolute inset-0 z-20 hidden md:motion-safe:block"
        data-family={EFFECT_PANELS[active].block.family}
      >
        <div className="sticky top-0 flex h-svh items-end">
          {/* An opaque band: a rising panel slides under the index instead of showing through it. */}
          <div data-index className="pointer-events-auto w-full border-t border-line bg-ink">
            <ol className="gutter mx-auto flex w-full max-w-[100rem] flex-wrap gap-x-7 gap-y-2 pt-2 pb-[3svh]">
              {EFFECT_PANELS.map(({ block }, i) => (
                <li key={block.id}>
                  <button
                    type="button"
                    onClick={() => jump(i)}
                    aria-current={i === active ? "step" : undefined}
                    className="group relative py-2 text-sm text-bone-3 transition-colors duration-200 ease-ui hover:text-bone aria-[current=step]:text-bone"
                  >
                    {block.name}
                    <span
                      aria-hidden
                      className="absolute inset-x-0 bottom-0 h-0.5 origin-left scale-x-0 bg-[var(--accent)] transition-transform duration-200 ease-ui group-aria-[current=step]:scale-x-100"
                    />
                  </button>
                </li>
              ))}
            </ol>
          </div>
        </div>
      </nav>
    </div>
  );
}
