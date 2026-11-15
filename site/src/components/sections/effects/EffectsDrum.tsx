"use client";

import { useRef, useState } from "react";
import { useGSAP } from "@gsap/react";
import { useLenis } from "lenis/react";
import { gsap, MQ, ScrollTrigger, setupGsap } from "@/lib/motion";
import VideoLoop from "@/components/ui/VideoLoop";
import Window from "@/components/ui/Window";
import { useDict } from "@/i18n/DictProvider";
import { EFFECT_PANELS } from "./panels";

const N = EFFECT_PANELS.length;
const STEP = 360 / N;
/** A face is this much taller than its window, which leaves a gap between neighbours on the drum. */
const FACE = 1.16;
/** Scroll each face costs, in viewport heights. */
const PER_FACE = 0.22;
const APOTHEM = 0.5 / Math.tan(Math.PI / N);

const pad = (n: number) => String(n).padStart(2, "0");
const dwell = (t: number) => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2);

/**
 * The eight effect editors on the faces of a drum. The scroll turns it one
 * face at a time; the face in front plays, the others wait on their poster.
 */
export default function EffectsDrum({ header }: { header: React.ReactNode }) {
  const scope = useRef<HTMLDivElement>(null);
  const trigger = useRef<ScrollTrigger | null>(null);
  const [active, setActive] = useState(0);
  const lenis = useLenis();
  const { block } = EFFECT_PANELS[active];
  const does = useDict().blocks;

  useGSAP(
    () => {
      setupGsap();
      const mm = gsap.matchMedia();
      mm.add(`${MQ.motion} and ${MQ.desktop}`, () => {
        const root = scope.current!;
        const drum = root.querySelector<HTMLElement>("[data-drum]")!;
        const faces = [...root.querySelectorAll<HTMLElement>("[data-face]")];
        let radius = 0;
        let shown = -1;

        const render = (progress: number) => {
          const raw = progress * (N - 1);
          const whole = Math.min(N - 2, Math.floor(raw));
          const p = whole + dwell(raw - whole);
          drum.style.transform = `translateZ(${-radius}px) rotateX(${p * STEP}deg)`;
          faces.forEach((face, i) => {
            const d = Math.abs(i - p);
            face.style.opacity = String(Math.max(0, Math.min(1, 1.2 - d * 0.85)));
            face.style.visibility = d > 1.9 ? "hidden" : "visible";
          });
          const front = Math.round(p);
          if (front !== shown) setActive((shown = front));
        };

        const measure = () => {
          const h = faces[0].offsetHeight;
          radius = h * APOTHEM;
          faces.forEach((face, i) => {
            face.style.transform = `translateY(-50%) rotateX(${-i * STEP}deg) translateZ(${radius}px)`;
          });
          render(trigger.current?.progress ?? 0);
        };

        const ro = new ResizeObserver(measure);
        ro.observe(faces[0]);
        trigger.current = ScrollTrigger.create({
          trigger: root,
          start: "top top",
          end: () => `+=${window.innerHeight * PER_FACE * (N - 1)}`,
          pin: true,
          scrub: 0.4,
          onUpdate: (self) => render(self.progress),
        });
        measure();

        return () => {
          ro.disconnect();
          trigger.current = null;
        };
      });
      return () => mm.revert();
    },
    { scope },
  );

  const go = (i: number) => {
    const st = trigger.current;
    if (!st) return;
    const y = st.start + (i / (N - 1)) * (st.end - st.start);
    if (lenis) lenis.scrollTo(y, { duration: 0.9 });
    else window.scrollTo({ top: y });
  };

  return (
    <div data-family={block.family}>
      <div
        ref={scope}
        className="gutter hidden md:motion-safe:flex md:motion-safe:h-svh md:motion-safe:flex-col md:motion-safe:justify-center md:motion-safe:pt-16"
      >
        <div className="mx-auto w-full max-w-[110rem]">
          {header}
          <div className="mt-[4svh] grid grid-cols-12 items-center gap-x-10">
            <div className="col-span-3">
              <ol className="flex flex-col">
                {EFFECT_PANELS.map(({ block: b }, i) => (
                  <li key={b.id}>
                    <button
                      type="button"
                      onClick={() => go(i)}
                      aria-current={i === active}
                      className="group flex w-full cursor-pointer items-baseline gap-4 py-[0.42rem] text-left text-lg text-bone-3 transition-colors duration-200 ease-ui hover:text-bone aria-[current=true]:text-bone"
                    >
                      <span className="label w-5 transition-colors duration-200 ease-ui group-aria-[current=true]:text-[var(--accent)]">
                        {pad(i + 1)}
                      </span>
                      {b.name}
                      <span
                        aria-hidden
                        className="h-px flex-1 origin-left scale-x-0 self-center bg-[var(--accent)] transition-transform duration-500 ease-out group-aria-[current=true]:scale-x-100"
                      />
                    </button>
                  </li>
                ))}
              </ol>
            </div>

            <div className="col-span-9">
              <div
                className="relative mx-auto max-w-[calc((100svh-25rem)*1.85)] [perspective:1500px]"
                style={{ aspectRatio: `1280 / ${296 * FACE * 2.05}` }}
              >
                <div data-drum className="absolute inset-0 [transform-style:preserve-3d]">
                  {EFFECT_PANELS.map(({ block: b, clip }, i) => (
                    <div
                      key={b.id}
                      data-face
                      aria-hidden={i !== active}
                      className="absolute inset-x-0 top-1/2 flex items-center [backface-visibility:hidden]"
                      style={{ aspectRatio: `1280 / ${296 * FACE}`, transform: i ? "translateY(-50%) scale(0)" : "translateY(-50%)" }}
                    >
                      <Window className="w-full">
                        <VideoLoop clip={clip} play={i === active} className="block h-auto w-full" />
                      </Window>
                    </div>
                  ))}
                </div>
              </div>
              <div className="mx-auto mt-2 flex max-w-[calc((100svh-25rem)*1.85)] items-baseline justify-between gap-8">
                <p key={block.id} className="drum-caption text-lead text-bone">
                  {does[block.id]}
                </p>
                <p className="label shrink-0">
                  <span className="text-[var(--accent)]">{pad(active + 1)}</span> / {pad(N)}
                </p>
              </div>
            </div>
          </div>
        </div>
      </div>

      <div className="py-24 md:motion-safe:hidden">
        <div className="gutter">{header}</div>
        <ul className="gutter mt-12 flex snap-x snap-mandatory gap-4 overflow-x-auto pb-4 [scrollbar-width:none]">
          {EFFECT_PANELS.map(({ block: b, clip }) => (
            <li key={b.id} data-family={b.family} className="w-[84vw] max-w-[36rem] shrink-0 snap-center">
              <Window>
                <VideoLoop clip={clip} className="block aspect-[2/1]! h-auto w-full object-cover object-left" />
              </Window>
              <h3 className="mt-5 flex items-center gap-2.5 text-xl font-medium">
                <span className="size-1.5 rounded-full bg-[var(--accent)]" aria-hidden />
                {b.name}
              </h3>
              <p className="mt-2 text-bone-2">{does[b.id]}</p>
            </li>
          ))}
        </ul>
      </div>
    </div>
  );
}
