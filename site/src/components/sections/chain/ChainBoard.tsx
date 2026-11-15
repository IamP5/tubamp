"use client";

import { useEffect, useReducer, useRef, useSyncExternalStore, type KeyboardEvent } from "react";
import { animate, MotionConfig, Reorder } from "motion/react";
import { useReducedMotion } from "@/components/SmoothScroll";
import { buttonClass } from "@/components/ui/Button";
import { CHAIN } from "@/data/site";
import { fill } from "@/i18n/config";
import { useDict } from "@/i18n/DictProvider";
import type { Dictionary } from "@/i18n/en";
import { EASE } from "@/lib/motion";
import Glyph from "./Glyph";
import { initialBoard, isHome, lit, reduce, type Action, type Board, type Note } from "./board";
import { blend, drawWave, samples } from "./draw";
import { isStage, type Stage } from "./signal";

const HOME: readonly Stage[] = CHAIN.map((b) => b.id).filter(isStage);
const BLOCKS = new Map(CHAIN.map((b) => [b.id, b]));
const name = (id: Stage) => BLOCKS.get(id)?.name ?? id;

/** The `board:` variant in globals.css is this same query: one row to drag needs the width and a mouse. */
const ROW = "(min-width: 64rem) and (hover: hover) and (pointer: fine)";
/** How long the board sits in view before the one demonstration, and how long its two tiles stay raised. */
const DEMO = { wait: 900, hold: 1100 } as const;
const SPRING = { type: "spring", stiffness: 420, damping: 34, mass: 0.8 } as const;
const MORPH = 0.5;

function caption(note: Note, copy: Dictionary["chain"]["board"]): string {
  switch (note.kind) {
    case "rest":
      return copy.rest;
    case "tail":
      return copy.tail;
    case "first":
      return fill(copy.first, { block: name(note.id) });
    case "after":
      return fill(copy.after, { block: name(note.id), other: name(note.other) });
  }
}

const subscribeRow = (cb: () => void) => {
  const mq = window.matchMedia(ROW);
  mq.addEventListener("change", cb);
  return () => mq.removeEventListener("change", cb);
};

export default function ChainBoard() {
  const [board, dispatch] = useReducer((b: Board, a: Action) => reduce(b, a, HOME), HOME, initialBoard);
  const foot = useRef<HTMLDivElement>(null);
  const copy = useDict().chain.board;
  const reduced = useReducedMotion();
  const row = useSyncExternalStore(subscribeRow, () => window.matchMedia(ROW).matches, () => false);
  const phase = board.phase.kind;
  const raised = lit(board.phase);
  const total = board.order.length;

  useEffect(() => {
    if (reduced || phase !== "waiting") return;
    let timer: ReturnType<typeof setTimeout> | undefined;
    const io = new IntersectionObserver(
      ([entry]) => {
        clearTimeout(timer);
        if (entry.isIntersecting) timer = setTimeout(() => dispatch({ type: "demo" }), DEMO.wait);
      },
      { threshold: 1 },
    );
    // The last line of the board: once it is on screen, so are the tiles and the waveform the swap changes.
    io.observe(foot.current!);
    return () => {
      io.disconnect();
      clearTimeout(timer);
    };
  }, [reduced, phase]);

  useEffect(() => {
    if (phase !== "demo") return;
    const timer = setTimeout(() => dispatch({ type: "demoEnd" }), DEMO.hold);
    return () => clearTimeout(timer);
  }, [phase]);

  const onKey = (id: Stage) => (e: KeyboardEvent<HTMLLIElement>) => {
    if (e.target !== e.currentTarget || e.altKey || e.ctrlKey || e.metaKey) return;
    const by = e.key === "ArrowLeft" || e.key === "ArrowUp" ? -1 : e.key === "ArrowRight" || e.key === "ArrowDown" ? 1 : 0;
    if (!by) return;
    // Also keeps Reorder's own arrow handling (move focus) from running.
    e.preventDefault();
    dispatch({ type: "step", id, by });
  };

  return (
    <MotionConfig reducedMotion="user">
      <div className="mt-10 flex flex-col gap-5 md:mt-12 md:gap-6">
        <div className="board:[perspective:2200px]">
          <div className="board:flex board:origin-bottom board:items-center board:gap-4 board:rounded-2xl board:border board:border-line board:bg-ink-1 board:px-7 board:py-11 board:[transform-style:preserve-3d] board:[transform:rotateX(24deg)]">
            <span aria-hidden className="label hidden board:block">
              In
            </span>
            <div className="relative min-w-0 flex-1 board:[transform-style:preserve-3d]">
              <span aria-hidden className="absolute inset-x-0 top-1/2 hidden h-px bg-line-strong board:block" />
              <Reorder.Group
                as="ol"
                axis="x"
                values={board.order as Stage[]}
                onReorder={(order: Stage[], id: Stage) => dispatch({ type: "arrange", order, id })}
                aria-label={copy.label}
                className="relative grid grid-cols-3 gap-2 sm:grid-cols-5 lg:grid-cols-9 board:flex board:gap-3 board:[transform-style:preserve-3d]"
              >
                {board.order.map((id, i) => {
                  const block = BLOCKS.get(id)!;
                  const up = raised.includes(id);
                  return (
                    <Reorder.Item
                      as="li"
                      key={id}
                      value={id}
                      drag={row ? "x" : false}
                      tabIndex={0}
                      data-family={block.family}
                      data-tile={id}
                      data-raised={up || undefined}
                      aria-label={fill(copy.position, { block: block.name, n: i + 1, total })}
                      onDragStart={() => dispatch({ type: "grab", id })}
                      onDragEnd={() => dispatch({ type: "drop" })}
                      onKeyDown={onKey(id)}
                      animate={{ z: up ? 56 : 0, scale: up ? 1.05 : 1 }}
                      transition={SPRING}
                      className="relative flex min-w-0 flex-col items-center gap-2 rounded-lg border border-line-strong bg-ink-2 px-1 pt-3 pb-2 transition-[border-color,box-shadow] duration-200 ease-ui select-none data-[raised]:border-[color-mix(in_oklab,var(--accent)_70%,transparent)] data-[raised]:shadow-[0_28px_48px_-14px_rgb(0_0_0/0.85)] board:flex-1 board:basis-0 board:cursor-grab board:py-4 board:active:cursor-grabbing"
                    >
                      <span className="label absolute top-1.5 left-2" aria-hidden>
                        {i + 1}
                      </span>
                      <span className="grid size-8 place-items-center rounded-md bg-[color-mix(in_oklab,var(--accent)_14%,transparent)] text-[var(--accent)] board:size-9">
                        <Glyph id={id} className="size-[18px] board:size-5" />
                      </span>
                      <span className="max-w-full truncate text-[0.8rem] text-bone xl:text-sm">{block.name}</span>
                      <span className="flex gap-1 board:hidden">
                        <Move label={fill(copy.earlier, { block: block.name })} off={i === 0} onMove={() => dispatch({ type: "step", id, by: -1 })} d="M10 3.5 5.5 8l4.5 4.5" />
                        <Move label={fill(copy.later, { block: block.name })} off={i === total - 1} onMove={() => dispatch({ type: "step", id, by: 1 })} d="M6 3.5 10.5 8 6 12.5" />
                      </span>
                    </Reorder.Item>
                  );
                })}
              </Reorder.Group>
            </div>
            <span aria-hidden className="label hidden board:block">
              Out
            </span>
          </div>
        </div>

        <Wave order={board.order} instant={reduced} />

        <div ref={foot} className="flex min-h-10 items-start justify-between gap-6">
          <p aria-live="polite" className="max-w-[48rem] text-base text-bone md:text-lead">
            <span key={caption(board.note, copy)} className="drum-caption block">
              {caption(board.note, copy)}
            </span>
          </p>
          {!isHome(board, HOME) && (
            <button type="button" onClick={() => dispatch({ type: "reset" })} className={buttonClass("ghost", "h-9! shrink-0 cursor-pointer px-4! text-sm!")}>
              {copy.reset}
            </button>
          )}
        </div>
      </div>
    </MotionConfig>
  );
}

function Move({ label, off, onMove, d }: { label: string; off: boolean; onMove: () => void; d: string }) {
  return (
    <button
      type="button"
      aria-label={label}
      // Not `disabled`: a tile that reaches the end keeps the focus on the button that moved it.
      aria-disabled={off}
      onClick={off ? undefined : onMove}
      className="grid size-9 place-items-center rounded-md text-bone-2 transition-colors duration-150 ease-ui hover:bg-ink-3 hover:text-bone aria-disabled:text-line-strong"
    >
      <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth={1.5} strokeLinecap="round" strokeLinejoin="round" aria-hidden>
        <path d={d} />
      </svg>
    </button>
  );
}

/** The signal the current order gives. It eases from the shape on screen, so a change mid-ease never jumps. */
function Wave({ order, instant }: { order: readonly Stage[]; instant: boolean }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const shown = useRef<{ from: Float32Array; to: Float32Array; mix: number } | null>(null);
  const latest = useRef(order);
  const label = useDict().chain.board.waveform;

  useEffect(() => {
    latest.current = order;
    const el = canvas.current!;
    const css = getComputedStyle(el);
    const ink = {
      signal: css.getPropertyValue("--color-signal").trim(),
      ghost: css.getPropertyValue("--color-bone-3").trim(),
      base: css.getPropertyValue("--color-line").trim(),
    };
    const span = () => (el.clientWidth < 640 ? 0.5 : 1);
    const paint = () => {
      const s = shown.current;
      if (s) drawWave(el, s.from, s.to, s.mix, ink);
    };
    const settle = () => {
      const to = samples(latest.current, span());
      shown.current = { from: to, to, mix: 1 };
      paint();
    };

    const to = samples(order, span());
    const prev = shown.current;
    if (instant || !prev || prev.to.length !== to.length) {
      settle();
    } else {
      const state = { from: blend(prev.from, prev.to, prev.mix), to, mix: 0 };
      shown.current = state;
      paint();
    }
    const morph =
      shown.current!.mix < 1
        ? animate(0, 1, {
            duration: MORPH,
            ease: EASE.inOut,
            onUpdate: (v) => {
              shown.current!.mix = v;
              paint();
            },
            // Land on the new shape itself, not on a sum that is only nearly it.
            onComplete: () => {
              shown.current = { from: to, to, mix: 1 };
              paint();
            },
          })
        : null;

    let width = el.clientWidth;
    const ro = new ResizeObserver(() => {
      if (el.clientWidth === width) return;
      width = el.clientWidth;
      morph?.stop();
      settle();
    });
    ro.observe(el);
    return () => {
      morph?.stop();
      ro.disconnect();
    };
  }, [order, instant]);

  return (
    <div className="relative h-[clamp(96px,17svh,180px)]">
      <canvas ref={canvas} role="img" aria-label={label} className="absolute inset-0 size-full" />
    </div>
  );
}
