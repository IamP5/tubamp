import { CHAPTERS } from "@/data/site";
import { CAPTURE_SEQUENCE, FILMS } from "@/data/media";
import ChapterLabel from "@/components/ui/ChapterLabel";
import CapturesStage from "./effects/CapturesStage";
import Reveal from "./effects/Reveal";
import ScrubFilm from "./captures/ScrubFilm";

const copy = CHAPTERS.captures;

export default function Captures() {
  return (
    <section id="captures" data-family="gain" aria-labelledby="captures-title" className="relative">
      <ScrubFilm sequence={CAPTURE_SEQUENCE} loop={FILMS.capture}>
        <div className="gutter mx-auto w-full max-w-[110rem]">
          <ChapterLabel>{copy.kicker}</ChapterLabel>
          <h2 id="captures-title" className="display mt-5 text-display">
            <span className="lead-in block">
              {copy.headline[0]}
            </span>
            <span className="block">
              {copy.headline[1]}
            </span>
          </h2>
          <p className="mt-6 max-w-[32rem] text-lg leading-relaxed text-bone-2">{copy.body}</p>
        </div>
      </ScrubFilm>

      <div className="gutter mx-auto grid max-w-[110rem] gap-y-12 pt-[6svh] pb-[8svh] lg:grid-cols-12 lg:items-center lg:gap-x-10">
        <div className="lg:order-2 lg:col-span-8">
          <CapturesStage />
        </div>
        <Reveal start="top 90%" stagger={0.07} className="lg:order-1 lg:col-span-4">
          <ul className="grid gap-x-10 sm:grid-cols-2 lg:grid-cols-1">
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
