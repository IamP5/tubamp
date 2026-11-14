"use client";

import { Fragment, useRef } from "react";
import { useGSAP } from "@gsap/react";
import { gsap, MQ, setupGsap } from "@/lib/motion";
import { CHAIN } from "@/data/site";

type Tile = { kind: "block"; name: string } | { kind: "slot"; n: number };

const name = (id: string) => CHAIN.find((b) => b.id === id)?.name ?? id;

const TILES: readonly Tile[] = [
  { kind: "block", name: name("gate") },
  { kind: "slot", n: 1 },
  { kind: "block", name: name("drive") },
  { kind: "block", name: name("amp") },
  { kind: "block", name: name("cab") },
  { kind: "slot", n: 2 },
  { kind: "block", name: name("eq") },
  { kind: "block", name: name("delay") },
  { kind: "slot", n: 3 },
  { kind: "block", name: name("reverb") },
];

const chunk = <T,>(list: readonly T[], size: number) =>
  Array.from({ length: Math.ceil(list.length / size) }, (_, i) => list.slice(i * size, i * size + size));

/** Row height and gap in px; the wrap path is drawn from the same numbers. */
const ROW = 52;
const GAP = 44;

function wrapPath(rows: number) {
  const height = rows * ROW + (rows - 1) * GAP;
  const y = (r: number) => ((r * (ROW + GAP) + ROW / 2) / height) * 100;
  const mid = (r: number) => ((r * (ROW + GAP) + ROW + GAP / 2) / height) * 100;
  return Array.from({ length: rows - 1 }, (_, r) => `M100 ${y(r)} V${mid(r)} H0 V${y(r + 1)}`).join(" ");
}

/** `tight` is the board in a narrow column: the slot is named inside its tile, where "Your plugin" would not fit. */
function Board({ perRow, className, tight = false }: { perRow: number; className: string; tight?: boolean }) {
  const rows = chunk(TILES, perRow);
  return (
    <div data-board className={`items-start gap-3 ${className}`}>
      <span className="label flex h-[52px] items-center">In</span>
      <div className="relative flex min-w-0 flex-1 flex-col" style={{ rowGap: GAP }}>
        <svg
          aria-hidden
          className="pointer-events-none absolute inset-0 size-full overflow-visible text-line-strong"
          viewBox="0 0 100 100"
          preserveAspectRatio="none"
        >
          <path d={wrapPath(rows.length)} fill="none" stroke="currentColor" strokeWidth={1} vectorEffect="non-scaling-stroke" />
        </svg>
        {rows.map((row, r) => (
          <div key={r} className="flex items-center" style={{ height: ROW }}>
            <span className="h-px w-3 flex-none bg-line-strong md:w-5" aria-hidden />
            {row.map((tile, t) => (
              <Fragment key={t}>
                {t > 0 && <span className="h-px w-3 flex-none bg-line-strong md:w-5" aria-hidden />}
                <span className="relative h-full min-w-0 flex-1 basis-0">
                  {tile.kind === "block" ? (
                    <span className="flex h-full items-center rounded-lg border border-line bg-ink-2 px-3 text-sm text-bone-2">
                      <span className="truncate">{tile.name}</span>
                    </span>
                  ) : (
                    <>
                      <span className="absolute inset-0 rounded-lg border border-dashed border-line-strong" aria-hidden />
                      <span
                        data-slot
                        className="relative flex h-full items-center rounded-lg border border-[var(--accent)] bg-ink-1 px-3 text-sm text-bone"
                      >
                        {tight ? (
                          <span className="label text-[var(--accent)]">AU {tile.n}</span>
                        ) : (
                          <>
                            <span className="truncate">Your plugin</span>
                            <span className="label absolute -top-5 left-0 text-[var(--accent)]">AU {tile.n}</span>
                          </>
                        )}
                      </span>
                    </>
                  )}
                </span>
              </Fragment>
            ))}
            {/* A short last row keeps its tiles the width of the others; the wire runs on to Out. */}
            <span
              className={`h-px bg-line-strong ${row.length < perRow ? "basis-0" : "w-3 flex-none md:w-5"}`}
              style={row.length < perRow ? { flexGrow: perRow - row.length } : undefined}
              aria-hidden
            />
          </div>
        ))}
      </div>
      <span className="label flex h-[52px] items-center self-end">Out</span>
    </div>
  );
}

export default function ChainSlots() {
  const scope = useRef<HTMLDivElement>(null);

  useGSAP(
    () => {
      setupGsap();
      const mm = gsap.matchMedia();
      mm.add(MQ.motion, () => {
        gsap.utils.toArray<HTMLElement>("[data-board]", scope.current).forEach((board) => {
          gsap.fromTo(
            board.querySelectorAll("[data-slot]"),
            { y: -40, opacity: 0 },
            {
              y: 0,
              opacity: 1,
              ease: "none",
              stagger: 0.35,
              scrollTrigger: { trigger: board, start: "top 88%", end: "top 40%", scrub: 0.6 },
            },
          );
        });
      });
      return () => mm.revert();
    },
    { scope },
  );

  return (
    <figure ref={scope} aria-label="A chain with three of your own Audio Unit plugins placed between the blocks.">
      <Board perRow={4} tight className="hidden lg:flex" />
      <Board perRow={5} className="hidden md:flex lg:hidden" />
      <Board perRow={2} className="flex md:hidden" />
    </figure>
  );
}
