"use client";

import Image from "next/image";
import { useEffect, useRef, useState, useSyncExternalStore } from "react";
import { motion, useScroll, useTransform } from "motion/react";
import ChapterLabel from "@/components/ui/ChapterLabel";
import VideoLoop from "@/components/ui/VideoLoop";
import Window from "@/components/ui/Window";
import { useReducedMotion } from "@/components/SmoothScroll";
import { buttonClass } from "@/components/ui/Button";
import { CLIPS, clipSrc } from "@/data/media";
import { CHAPTERS } from "@/data/site";

const copy = CHAPTERS.demo;
const DEMO_URL = "/demo/index.html";
const FRAME = { width: 1280, height: 800 };
const WIDE = "(min-width: 900px)";

/**
 * poster: nothing loaded, the Start button over a still.
 * missing: the demo bundle is not deployed; the presets loop stands in.
 * live: the editor has the pointer and the keyboard.
 * idle: the editor is loaded but the page has the wheel back; a click hands it over again.
 */
type Stage = "poster" | "missing" | "live" | "idle";

const subscribeWide = (cb: () => void) => {
  const mq = window.matchMedia(WIDE);
  mq.addEventListener("change", cb);
  return () => mq.removeEventListener("change", cb);
};

function useWide() {
  return useSyncExternalStore(subscribeWide, () => window.matchMedia(WIDE).matches, () => false);
}

export default function Demo() {
  const wide = useWide();
  const [line1, line2] = copy.headline;
  const turn = "touch it.";

  return (
    <section id="try" aria-labelledby="try-title" className="gutter relative bg-ink py-[clamp(6rem,12vw,11rem)]">
      <div className="mx-auto max-w-[110rem]">
        <ChapterLabel index={copy.index}>{copy.kicker}</ChapterLabel>
        <div className="mt-8 grid gap-8 lg:grid-cols-12 lg:items-end">
          <h2 id="try-title" className="display text-h2 lg:col-span-8">
            {line1}
            <br />
            {line2.replace(turn, "")}
            <span className="serif-accent">{turn}</span>
          </h2>
          <p className="max-w-[34rem] text-lg text-bone-2 lg:col-span-4 lg:pb-2">{copy.body}</p>
        </div>
        <div className="mt-[clamp(3rem,6vw,5rem)]">{wide ? <Stage /> : <Narrow />}</div>
      </div>
    </section>
  );
}

function Narrow() {
  return (
    <figure className="flex flex-col gap-4">
      <Window>
        <VideoLoop clip={CLIPS.presets} className="block h-auto w-full" />
      </Window>
      <figcaption className="text-sm text-bone-3">The live editor needs a wider screen.</figcaption>
    </figure>
  );
}

function Stage() {
  const [stage, setStage] = useState<Stage>("poster");
  const [scale, setScale] = useState(0);
  const boxRef = useRef<HTMLDivElement>(null);
  const frameRef = useRef<HTMLIFrameElement>(null);
  const reduced = useReducedMotion();

  const { scrollYProgress } = useScroll({ target: boxRef, offset: ["start end", "start 30%"] });
  const grow = useTransform(scrollYProgress, [0, 1], [0.9, 1]);

  useEffect(() => {
    const box = boxRef.current;
    if (!box) return;
    const ro = new ResizeObserver(([entry]) => setScale(entry.contentRect.width / FRAME.width));
    ro.observe(box);
    return () => ro.disconnect();
  }, []);

  useEffect(() => {
    const box = boxRef.current;
    if (!box) return;
    const io = new IntersectionObserver(
      ([entry]) => {
        if (!entry.isIntersecting) return;
        io.disconnect();
        fetch(DEMO_URL, { method: "HEAD" })
          .then((res) => !res.ok && setStage("missing"))
          .catch(() => setStage("missing"));
      },
      { rootMargin: "600px 0px" },
    );
    io.observe(box);
    return () => io.disconnect();
  }, []);

  useEffect(() => {
    if (stage !== "live") return;
    const release = () => setStage("idle");
    const onKey = (e: KeyboardEvent) => e.key === "Escape" && release();
    const onDown = (e: PointerEvent) => {
      if (!boxRef.current?.contains(e.target as Node)) release();
    };
    const inner = frameRef.current?.contentWindow;
    document.addEventListener("keydown", onKey);
    document.addEventListener("pointerdown", onDown);
    inner?.addEventListener("keydown", onKey);
    return () => {
      document.removeEventListener("keydown", onKey);
      document.removeEventListener("pointerdown", onDown);
      inner?.removeEventListener("keydown", onKey);
    };
  }, [stage]);

  const loaded = stage === "live" || stage === "idle";
  const start = () => setStage("live");

  return (
    <motion.div
      style={reduced ? undefined : { scale: grow }}
      className="mx-auto max-w-[calc((100svh-9rem)*1.6)] min-w-[min(100%,52rem)] origin-top"
    >
      <Window className="relative">
        <div
          ref={boxRef}
          className="relative w-full overflow-hidden"
          style={{ aspectRatio: `${FRAME.width} / ${FRAME.height}` }}
        >
          {loaded && scale > 0 && (
            <iframe
              ref={frameRef}
              src={DEMO_URL}
              title="The tubamp editor, running in the page"
              width={FRAME.width}
              height={FRAME.height}
              onLoad={() => frameRef.current?.focus()}
              className="absolute top-0 left-0 origin-top-left border-0"
              style={{
                transform: `scale(${scale})`,
                pointerEvents: stage === "live" ? "auto" : "none",
              }}
            />
          )}

          {stage === "idle" && (
            <button
              type="button"
              onClick={start}
              aria-label="Use the editor"
              className="absolute inset-0 cursor-pointer"
            />
          )}

          {stage === "missing" && <VideoLoop clip={CLIPS.presets} className="absolute inset-0 h-full w-full" />}

          {stage === "poster" && <Poster onStart={start} />}
        </div>
      </Window>
      <p className="mt-4 h-5 text-sm text-bone-3" aria-live="polite">
        {stage === "live" && "Press Esc or click outside the editor to scroll the page."}
        {stage === "idle" && "Click the editor to use it again."}
        {stage === "missing" && "The live editor is not in this build of the page. Here it is on video."}
      </p>
    </motion.div>
  );
}

function Poster({ onStart }: { onStart: () => void }) {
  const [still, setStill] = useState(true);
  return (
    <div className="absolute inset-0">
      {still && (
        <Image
          src={clipSrc(CLIPS.presets).poster}
          alt=""
          fill
          sizes="(min-width: 1760px) 1640px, 92vw"
          className="object-cover opacity-60"
          onError={() => setStill(false)}
        />
      )}
      <div className="absolute inset-0 bg-gradient-to-t from-ink via-ink/50 to-ink/10" />
      <div className="absolute inset-0 flex items-center justify-center">
        <button type="button" onClick={onStart} className={buttonClass("signal")}>
          <svg width="14" height="14" viewBox="0 0 16 16" fill="currentColor" aria-hidden>
            <path d="M4 2.8v10.4a.8.8 0 0 0 1.2.7l8.6-5.2a.8.8 0 0 0 0-1.4L5.2 2.1A.8.8 0 0 0 4 2.8Z" />
          </svg>
          Start the editor
        </button>
      </div>
    </div>
  );
}
