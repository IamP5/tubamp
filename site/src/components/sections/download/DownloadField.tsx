import { CHAPTERS, SITE } from "@/data/site";
import type { Release } from "@/lib/release-schema";
import ArchButtons from "./ArchButtons";
import Curtain from "./Curtain";
import { macOS, releaseDate } from "./format";
import { ArrowOut, fieldButton } from "./parts";

const copy = CHAPTERS.download;

/** The page's output: the amber field with the installers, or the way to GitHub when there is no release. */
export default function DownloadField({ release, id = "download" }: { release: Release | null; id?: string }) {
  const requirements = [
    release ? SITE.platforms.replace(/^macOS [\d.]+/, macOS(release.minimumMacOS)) : SITE.platforms,
    SITE.hosts,
  ];

  return (
    <section
      id={id}
      aria-labelledby={`${id}-title`}
      className="relative bg-ink [--accent:var(--color-on-signal)]"
    >
      <Curtain className="bg-signal text-on-signal">
        <div className="gutter mx-auto max-w-[110rem] pt-[clamp(5rem,11vw,10rem)] pb-[clamp(4rem,8vw,7rem)]">
          <p className="flex items-center gap-3 font-mono text-xs tabular-nums text-on-signal/75">
            <span className="text-on-signal">{copy.index}</span>
            <span className="h-px w-8 bg-on-signal/40" aria-hidden />
            <span>{copy.kicker}</span>
          </p>

          <h2 id={`${id}-title`} data-rise className="display mt-6 -ml-[0.04em] text-[clamp(4.5rem,26vw,8rem)] sm:text-mega">
            {copy.headline[0]}
          </h2>

          <div className="mt-[clamp(3rem,6vw,5.5rem)] grid gap-10 border-t border-on-signal/25 pt-10 lg:grid-cols-12 lg:gap-8">
            <div className="flex flex-col gap-6 lg:col-span-5">
              <p className="max-w-[34rem] text-lead leading-snug">{copy.body}</p>
              {release && (
                <p className="flex flex-wrap items-baseline gap-x-4 gap-y-2 text-sm">
                  <span className="font-mono tabular-nums">
                    v{release.version}, {releaseDate(release.publishedAt)}
                  </span>
                  <a
                    href={release.url}
                    className="underline decoration-on-signal/40 underline-offset-4 transition-colors duration-150 ease-ui hover:decoration-on-signal"
                  >
                    What changed
                  </a>
                </p>
              )}
            </div>

            <div className="lg:col-span-6 lg:col-start-7">
              {release ? (
                <ArchButtons downloads={release.downloads} />
              ) : (
                // A route handler that redirects to the installer: next/link would prefetch it.
                // eslint-disable-next-line @next/next/no-html-link-for-pages
                <a href="/download/arm64" className={`${fieldButton(true)} sm:max-w-sm`}>
                  Get it on GitHub
                  <ArrowOut />
                </a>
              )}
            </div>
          </div>

          <div className="mt-16 grid gap-10 border-t border-on-signal/25 pt-10 lg:grid-cols-12 lg:gap-8">
            <div className="lg:col-span-5">
              <h3 className="text-sm font-semibold">Requirements</h3>
              <ul className="mt-4 flex flex-col gap-1.5 text-base text-on-signal/85">
                {requirements.map((line) => (
                  <li key={line}>{line}</li>
                ))}
              </ul>
            </div>

            {release && (
              <div className="lg:col-span-6 lg:col-start-7">
                <h3 className="text-sm font-semibold">Install</h3>
                <ol className="mt-4 flex flex-col gap-3">
                  {copy.steps.map((step, i) => (
                    <li key={step} className="flex gap-4 text-base">
                      <span className="w-4 shrink-0 pt-[0.2em] font-mono text-xs tabular-nums text-on-signal/70" aria-hidden>
                        {i + 1}
                      </span>
                      <span>{step}</span>
                    </li>
                  ))}
                </ol>
                {!release.signed && (
                  <p className="mt-8 max-w-[34rem] border-l-2 border-on-signal/30 pl-4 text-sm leading-relaxed text-on-signal/80">
                    {copy.unsigned}
                  </p>
                )}
              </div>
            )}
          </div>
        </div>
      </Curtain>
    </section>
  );
}
