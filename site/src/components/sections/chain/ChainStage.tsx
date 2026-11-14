"use client";

import { useRef, type ReactNode } from "react";
import { useGSAP } from "@gsap/react";
import { gsap, MQ, ScrollTrigger, setupGsap } from "@/lib/motion";
import { CHAIN, CHAPTERS } from "@/data/site";
import Glyph from "./Glyph";
import { pad } from "./copy";
import { drawWave, type Ink } from "./draw";
import { BLOCKS, PINNED, SWAP, TOTAL, finalScene, sceneAt, type Scene } from "./scene";
import { isStage } from "./signal";
import { buildWire, type Rect, type Wire } from "./wire";

const ORDER = CHAIN.map((b) => b.id).filter(isStage);

/** Where the wire leaves the In jack, in px from the board's left edge. */
const WIRE_START = 20;

const clamp01 = (v: number) => (v < 0 ? 0 : v > 1 ? 1 : v);
const expoOut = (t: number) => (t >= 1 ? 1 : 1 - Math.pow(2, -10 * t));

type Layout = { rects: Rect[]; wire: Wire; total: number; ink: Ink; span: number };

export default function ChainStage({ header, after }: { header: ReactNode; after: ReactNode }) {
  const root = useRef<HTMLDivElement>(null);
  const pin = useRef<HTMLDivElement>(null);
  const stage = useRef<HTMLDivElement>(null);
  const board = useRef<HTMLDivElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);
  const wirePath = useRef<SVGPathElement>(null);
  const jackIn = useRef<HTMLSpanElement>(null);
  const jackOut = useRef<HTMLSpanElement>(null);

  useGSAP(
    () => {
      setupGsap();
      const el = root.current!;
      const path = wirePath.current!;
      const inJack = jackIn.current!;
      const outJack = jackOut.current!;
      const wave = canvas.current!;
      const boardEl = board.current!;
      const tiles = [...el.querySelectorAll<HTMLElement>("[data-tile]")];
      const captions = [...el.querySelectorAll<HTMLElement>("[data-caption]")];
      const rises = [...el.querySelectorAll<HTMLElement>("[data-rise]")];
      let layout: Layout | null = null;
      let current = finalScene(ORDER);
      let animateHeader = false;

      const measure = () => {
        const rects = tiles.map((t) => ({ x: t.offsetLeft, y: t.offsetTop, w: t.offsetWidth, h: t.offsetHeight }));
        const wire = buildWire(rects, WIRE_START);
        path.setAttribute("d", wire.d);
        inJack.style.top = `${rects[0].y + rects[0].h / 2}px`;
        const css = getComputedStyle(el);
        const ink = {
          signal: css.getPropertyValue("--color-signal").trim(),
          ghost: css.getPropertyValue("--color-bone-3").trim(),
          base: css.getPropertyValue("--color-line").trim(),
        };
        layout = { rects, wire, total: path.getTotalLength(), ink, span: boardEl.clientWidth < 640 ? 0.5 : 1 };
      };

      const render = (scene: Scene) => {
        current = scene;
        if (!layout) return;
        const { rects, wire, total, ink, span } = layout;

        rises.forEach((r, j) => {
          if (!animateHeader) {
            r.style.opacity = "";
            r.style.transform = "";
            return;
          }
          const p = expoOut(clamp01((scene.header - j * 0.12) / 0.5));
          r.style.opacity = String(p);
          r.style.transform = `translate3d(0, ${(1 - p) * 56}px, 0)`;
        });

        const i = Math.min(Math.floor(scene.wire), wire.stops.length - 2);
        const len = wire.stops[i] + (wire.stops[i + 1] - wire.stops[i]) * (scene.wire - i);
        path.style.strokeDasharray = `${len} ${total + 100}`;
        path.style.opacity = String(scene.chrome);
        const end = path.getPointAtLength(len);
        outJack.style.transform = `translate3d(${end.x - 3}px, ${end.y - 6}px, 0)`;
        outJack.style.opacity = String(scene.chrome);
        inJack.style.opacity = String(scene.chrome);

        const [a, b] = SWAP;
        const dx = rects[b].x - rects[a].x;
        const dy = rects[b].y - rects[a].y;
        const lift = Math.sin(Math.PI * scene.swap);
        const arc = lift * rects[a].w * 0.12;
        tiles.forEach((tile, k) => {
          const t = scene.tiles[k];
          let x = 0;
          let y = -t.drop * rects[k].h * 1.9;
          if (k === a) {
            x = dx * scene.swap + arc;
            y += dy * scene.swap;
          } else if (k === b) {
            x = -dx * scene.swap - arc;
            y -= dy * scene.swap;
          }
          const pass = k === b ? 1 + lift * 0.05 : k === a ? 1 - lift * 0.05 : 1;
          tile.style.transform = `translate3d(${x}px, ${y}px, 0) scale(${(1 - t.drop * 0.06) * pass})`;
          tile.style.zIndex = k === b && lift > 0 ? "2" : "";
          tile.style.opacity = String(t.opacity);
          const active = scene.caption === k || (scene.caption === BLOCKS && (k === a || k === b));
          tile.toggleAttribute("data-active", active);
        });

        captions.forEach((c, k) => c.toggleAttribute("data-on", k === scene.caption));
        const family = CHAIN[scene.caption]?.family;
        if (family) el.dataset.family = family;
        else delete el.dataset.family;

        drawWave(wave, scene, ink, span);
      };

      measure();
      render(current);
      const ro = new ResizeObserver(() => {
        measure();
        render(current);
      });
      ro.observe(boardEl);

      const mm = gsap.matchMedia();
      mm.add({ motion: MQ.motion, wide: "(min-width: 1024px)" }, (ctx) => {
        const { motion, wide } = ctx.conditions as { motion: boolean; wide: boolean };
        if (!motion) {
          render(finalScene(ORDER));
          return;
        }
        animateHeader = wide;
        const target = wide ? pin.current! : stage.current!;
        const pinned = ScrollTrigger.create({
          trigger: target,
          start: "top top",
          end: () => `+=${PINNED * window.innerHeight}`,
          pin: true,
          pinSpacing: true,
          invalidateOnRefresh: true,
        });
        const proxy = { u: 0 };
        gsap.to(proxy, {
          u: TOTAL,
          ease: "none",
          onUpdate: () => render(sceneAt(proxy.u, ORDER)),
          scrollTrigger: {
            trigger: target,
            start: "top 80%",
            end: () => pinned.end,
            scrub: 0.4,
            invalidateOnRefresh: true,
          },
        });
        render(sceneAt(0, ORDER));

        const handover = el.querySelector<HTMLElement>("[data-handover]");
        if (handover) {
          gsap.fromTo(
            handover,
            { scale: 0.9, yPercent: 8, transformOrigin: "50% 0%" },
            {
              scale: 1,
              yPercent: 0,
              ease: "none",
              scrollTrigger: { trigger: handover, start: "top bottom", end: "top 30%", scrub: true },
            },
          );
        }

        return () => {
          animateHeader = false;
          if (el.isConnected) render(finalScene(ORDER));
        };
      });

      return () => {
        ro.disconnect();
        mm.revert();
      };
    },
    { scope: root },
  );

  return (
    <div ref={root}>
      <div
        ref={pin}
        className="flex flex-col gap-10 md:gap-14 lg:motion-safe:h-svh lg:motion-safe:justify-center lg:motion-safe:gap-[clamp(2rem,6svh,4.5rem)] lg:motion-safe:pt-16"
      >
        {header}
        <div
          ref={stage}
          className="flex flex-col gap-6 md:gap-8 max-lg:motion-safe:min-h-svh max-lg:motion-safe:justify-center"
        >
          <div className="relative h-[clamp(104px,19svh,200px)]" aria-hidden>
            <canvas ref={canvas} className="absolute inset-0 size-full" />
          </div>

          <div ref={board} className="relative pr-8 pl-7 md:pl-10">
            <svg aria-hidden className="pointer-events-none absolute inset-0 size-full overflow-visible">
              <path ref={wirePath} fill="none" className="stroke-bone-3/60" strokeWidth={1.25} />
            </svg>
            <span ref={jackIn} aria-hidden className="label absolute left-0 -translate-y-1/2">
              In
            </span>
            <span
              ref={jackOut}
              aria-hidden
              className="label absolute top-0 left-0 z-10 flex items-center gap-1.5 whitespace-nowrap will-change-transform"
            >
              <span className="size-1.5 rounded-full bg-bone-3" />
              Out
            </span>
            <ol
              aria-label="The default chain"
              className="grid grid-cols-2 gap-x-3 gap-y-6 md:grid-cols-3 md:gap-x-6 md:gap-y-10 lg:grid-cols-5 lg:gap-y-11 xl:grid-cols-6 xl:gap-x-8"
            >
              {CHAIN.map((block) => (
                <li
                  key={block.id}
                  data-tile
                  data-family={block.family}
                  className="relative flex h-12 items-center gap-2.5 rounded-lg border border-line-strong bg-ink-1 px-2.5 transition-[border-color] duration-200 ease-ui will-change-transform data-[active]:border-[color-mix(in_oklab,var(--accent)_65%,transparent)] md:h-14 md:gap-3 md:px-3"
                >
                  <span className="grid size-7 shrink-0 place-items-center rounded-md bg-[color-mix(in_oklab,var(--accent)_14%,transparent)] text-[var(--accent)] md:size-8">
                    <Glyph id={block.id} className="size-4 md:size-[18px]" />
                  </span>
                  <span className="truncate text-sm text-bone md:text-[0.95rem]">{block.name}</span>
                </li>
              ))}
            </ol>
          </div>

          <div className="relative h-16 motion-reduce:hidden md:h-9">
            {[...CHAIN.map((b, i) => ({ index: pad(i + 1), title: `${b.name}.`, body: b.does })), { index: "", ...CHAPTERS.chain.swap }].map(
              (c) => (
                <p
                  key={c.title}
                  data-caption
                  className="absolute inset-x-0 top-0 max-w-[48rem] translate-y-2 text-base opacity-0 transition-[opacity,translate] duration-240 ease-ui data-[on]:translate-y-0 data-[on]:opacity-100 md:text-lead"
                >
                  {c.index && (
                    <span className="label mr-3 align-[0.2em]">
                      <span className="text-[var(--accent)] transition-colors duration-200">{c.index}</span>
                    </span>
                  )}
                  <span className="text-bone">{c.title}</span> <span className="text-bone-2">{c.body}</span>
                </p>
              ),
            )}
          </div>
        </div>
      </div>
      {after}
    </div>
  );
}
