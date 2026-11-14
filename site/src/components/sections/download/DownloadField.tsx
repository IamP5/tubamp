import VideoLoop from "@/components/ui/VideoLoop";
import ChapterLabel from "@/components/ui/ChapterLabel";
import { FILMS } from "@/data/media";
import { CHAPTERS, FAQ, SITE } from "@/data/site";
import type { Release } from "@/lib/release-schema";
import Reveal, { Line } from "../effects/Reveal";
import ArchButtons from "./ArchButtons";
import Backdrop from "./Backdrop";
import { installSteps, macOS, releaseDate, unsigned } from "./format";
import { ArrowOut, fieldButton } from "./parts";

const copy = CHAPTERS.download;

export default function DownloadField({ release, id = "download" }: { release: Release | null; id?: string }) {
  const requirements = [
    release ? SITE.platforms.replace(/^macOS [\d.]+/, macOS(release.minimumMacOS)) : SITE.platforms,
    SITE.hosts,
  ];
  const gatekeeper = unsigned(release);
  const questions = [...(gatekeeper ? [copy.gatekeeper] : []), ...FAQ];
  const link =
    "text-bone-2 underline decoration-line-strong underline-offset-4 transition-colors duration-150 ease-ui hover:text-bone hover:decoration-bone";

  return (
    <section id={id} aria-labelledby={`${id}-title`} className="relative isolate overflow-hidden bg-ink">
      <Backdrop>
        <VideoLoop clip={FILMS.speaker} className="size-full object-cover" />
      </Backdrop>

      <div className="gutter mx-auto max-w-[80rem] pt-[clamp(6rem,12vw,10rem)] pb-[clamp(4rem,8vw,7rem)]">
        <div className="flex flex-col items-center text-center">
          <ChapterLabel>{copy.kicker}</ChapterLabel>
          <Reveal className="mt-5">
            <h2 id={`${id}-title`} className="display text-display">
              {copy.headline.map((line) => (
                <Line key={line}>{line}</Line>
              ))}
            </h2>
          </Reveal>
          <p className="mt-6 max-w-[34rem] text-lead leading-snug text-bone-2">{copy.body}</p>
        </div>

        <div className="mt-12 rounded-2xl border border-line-strong bg-ink-1 p-6 sm:p-10">
          {release ? (
            <ArchButtons downloads={release.downloads} />
          ) : (
            // A route handler that redirects to the installer: next/link would prefetch it.
            // eslint-disable-next-line @next/next/no-html-link-for-pages
            <a href="/download/arm64" className={`${fieldButton(true)} mx-auto flex! sm:max-w-sm`}>
              Get it on GitHub
              <ArrowOut />
            </a>
          )}

          <p className="mt-7 text-center text-sm text-bone-3">
            {copy.free} {gatekeeper && `${copy.unsignedNote} `}
            <a href={SITE.repoUrl} className={link}>
              {copy.source}
            </a>
          </p>

          <div className="mt-8 grid gap-10 border-t border-line pt-8 md:grid-cols-2">
            <div>
              <h3 className="text-sm font-medium text-bone">Install</h3>
              <ol className="mt-4 flex flex-col gap-3">
                {installSteps(copy.steps, gatekeeper).map((step, i) => (
                  <li key={step} className="flex gap-4 text-bone-2">
                    <span className="label w-4 shrink-0 pt-[0.45em] text-signal" aria-hidden>
                      {i + 1}
                    </span>
                    <span>{step}</span>
                  </li>
                ))}
              </ol>
            </div>
            <div>
              <h3 className="text-sm font-medium text-bone">Requirements</h3>
              <ul className="mt-4 flex flex-col gap-1.5 text-bone-2">
                {requirements.map((line) => (
                  <li key={line}>{line}</li>
                ))}
              </ul>
              {release && (
                <p className="mt-5 flex flex-wrap items-baseline gap-x-4 gap-y-2 text-sm text-bone-3">
                  <span className="font-mono tabular-nums">
                    v{release.version}, {releaseDate(release.publishedAt)}
                  </span>
                  <a href={release.url} className={link}>
                    What changed
                  </a>
                </p>
              )}
            </div>
          </div>
        </div>

        <div id="faq" className="mt-[clamp(4rem,8vw,7rem)] grid scroll-mt-24 gap-x-10 gap-y-8 md:grid-cols-12">
          <h3 className="display text-h3 md:col-span-4">Questions</h3>
          <ul className="md:col-span-8">
            {questions.map((item, i) => (
              <li key={item.q} className="border-b border-line first:border-t">
                <details className="group" open={i === 0}>
                  <summary className="flex cursor-pointer list-none items-center justify-between gap-6 py-5 text-lg text-bone [&::-webkit-details-marker]:hidden">
                    {item.q}
                    <svg
                      aria-hidden
                      viewBox="0 0 16 16"
                      className="size-4 shrink-0 text-bone-3 transition-transform duration-200 ease-ui group-open:rotate-45"
                      fill="none"
                      stroke="currentColor"
                      strokeWidth="1.5"
                      strokeLinecap="round"
                    >
                      <path d="M8 2.5v11M2.5 8h11" />
                    </svg>
                  </summary>
                  <p className="max-w-[40rem] pb-6 leading-relaxed text-bone-2">{item.a}</p>
                </details>
              </li>
            ))}
          </ul>
        </div>
      </div>
    </section>
  );
}
