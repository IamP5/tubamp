import { Fragment } from "react";
import DownloadCta from "@/components/sections/download/DownloadCta";
import { SITE } from "@/data/site";
import HeroStage from "./HeroStage";

const turn = SITE.tagline.indexOf(" is ");
const LEAD = SITE.tagline.slice(0, turn).split(" ");
const ACCENT = SITE.tagline.slice(turn + 1).split(" ");

function Words({ words, className = "" }: { words: string[]; className?: string }) {
  return words.map((word, i) => (
    <Fragment key={i}>
      {i > 0 && " "}
      <span className="-my-[0.14em] inline-block overflow-clip py-[0.14em] align-bottom">
        <span data-word className={`inline-block ${className}`}>
          {word}
        </span>
      </span>
    </Fragment>
  ));
}

export default function HeroA({ version }: { version: string | null }) {
  return (
    <section aria-labelledby="hero-title">
      <HeroStage
        line={<p className="text-lead text-bone-2">{SITE.line}</p>}
        headline={
          <h1 id="hero-title" className="display text-display md:motion-reduce:text-h2">
            <Words words={LEAD} />{" "}
            <br />
            <Words words={ACCENT} className="serif-accent pr-[0.06em]" />
          </h1>
        }
        description={<p className="max-w-[34rem] text-lead text-bone-2">{SITE.what}</p>}
        actions={
          <div className="flex flex-wrap items-center gap-x-6 gap-y-4">
            <div className="flex items-center gap-4">
              <DownloadCta />
              {version && <span className="label">v{version}</span>}
            </div>
            <a
              href="#chain"
              className="group/more inline-flex h-12 items-center gap-2 text-[0.95rem] text-bone-2 transition-colors duration-200 ease-ui hover:text-bone"
            >
              See it move
              <svg
                aria-hidden
                viewBox="0 0 16 16"
                className="size-4 transition-transform duration-200 ease-ui group-hover/more:translate-y-0.5"
                fill="none"
                stroke="currentColor"
                strokeWidth="1.5"
                strokeLinecap="round"
                strokeLinejoin="round"
              >
                <path d="M8 3v10M4 9l4 4 4-4" />
              </svg>
            </a>
          </div>
        }
      />
    </section>
  );
}
