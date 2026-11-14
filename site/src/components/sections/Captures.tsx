import { CHAPTERS } from "@/data/site";
import ChapterLabel from "@/components/ui/ChapterLabel";
import CapturesStage from "./effects/CapturesStage";
import Reveal, { Line } from "./effects/Reveal";

const copy = CHAPTERS.captures;

export default function Captures() {
  return (
    <section id="captures" data-family="gain" className="gutter relative py-[12svh]">
      <div className="mx-auto max-w-[100rem]">
        <header className="grid gap-y-8 md:grid-cols-12 md:items-end md:gap-x-10">
          <div className="md:col-span-7">
            <ChapterLabel index={copy.index}>{copy.kicker}</ChapterLabel>
            <Reveal className="mt-8">
              <h2 className="display text-h2 text-balance">
                <Line>{copy.headline[0]}</Line>
                <Line className="serif-accent">{copy.headline[1]}</Line>
              </h2>
            </Reveal>
          </div>
          <p className="max-w-[34rem] text-lg leading-relaxed text-bone-2 md:col-span-5 md:justify-self-end">
            {copy.body}
          </p>
        </header>

        <div className="mt-[8svh] md:mt-[10svh]">
          <CapturesStage />
        </div>

        <Reveal start="top 90%" stagger={0.07}>
          <ul className="mt-14 grid gap-x-10 sm:grid-cols-2 md:mt-[10svh] lg:grid-cols-4">
            {copy.facts.map((fact) => (
              <li key={fact} data-line className="overflow-hidden border-t border-line">
                <span className="block pt-4 pb-6 text-[0.95rem] leading-snug text-bone">{fact}</span>
              </li>
            ))}
          </ul>
        </Reveal>
      </div>
    </section>
  );
}
