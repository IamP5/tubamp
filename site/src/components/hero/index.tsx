import DownloadCta from "@/components/sections/download/DownloadCta";
import { releaseMeta } from "@/components/sections/download/format";
import SignalField from "@/components/ui/SignalField";
import { getDictionary } from "@/i18n/dictionaries";
import type { Release } from "@/lib/release-schema";
import HeroStage from "./HeroStage";

export default async function Hero({ release }: { release: Release | null }) {
  const t = await getDictionary();
  const [setUp, landing] = t.hero.headline;
  return (
    <section id="top" aria-labelledby="hero-title" className="relative isolate overflow-x-clip">
      <SignalField className="absolute inset-x-0 bottom-0 -z-10 h-[62%] w-full" />
      <div className="gutter mx-auto grid max-w-[110rem] items-center gap-x-10 gap-y-16 pt-28 pb-24 lg:min-h-svh lg:grid-cols-12 lg:pt-24 lg:pb-20">
        <div className="lg:col-span-5">
          <p className="hero-in flex items-center gap-2.5 text-sm text-bone-2 [--i:0]">
            <span className="size-1.5 rounded-full bg-signal" aria-hidden />
            {t.hero.line}
          </p>
          <h1 id="hero-title" className="hero-in display mt-6 text-display text-balance [--i:1]">
            <span className="lead-in">{setUp}</span>
            <br />
            {landing}
          </h1>
          <p className="hero-in mt-6 max-w-[30rem] text-lead leading-snug text-bone-2 [--i:2]">{t.hero.what}</p>

          <div className="hero-in mt-9 flex flex-wrap items-center gap-x-6 gap-y-4 [--i:3]">
            <DownloadCta label={t.hero.download} className="h-14! px-8! text-base!" />
            <a
              href="#try"
              className="group hidden h-12 items-center gap-2.5 text-[0.95rem] text-bone transition-colors duration-200 ease-ui hover:text-signal lg:inline-flex"
            >
              <span className="grid size-8 place-items-center rounded-full border border-line-strong transition-colors duration-200 ease-ui group-hover:border-signal">
                <svg width="10" height="10" viewBox="0 0 16 16" fill="currentColor" aria-hidden>
                  <path d="M4 2.8v10.4a.8.8 0 0 0 1.2.7l8.6-5.2a.8.8 0 0 0 0-1.4L5.2 2.1A.8.8 0 0 0 4 2.8Z" />
                </svg>
              </span>
              {t.hero.tryIt}
            </a>
          </div>
          <ul className="hero-in mt-6 flex flex-wrap gap-x-5 gap-y-1.5 text-sm text-bone-3 [--i:4]">
            {releaseMeta(release, t.release).map((item) => (
              <li key={item} className="flex items-center gap-5 after:size-[3px] after:rounded-full after:bg-line-strong last:after:hidden">
                {item}
              </li>
            ))}
          </ul>
        </div>

        <div className="lg:col-span-7 lg:pr-[2%] lg:pl-[8%]">
          <HeroStage />
        </div>
      </div>
    </section>
  );
}
