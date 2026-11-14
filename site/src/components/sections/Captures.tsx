import { CHAPTERS } from "@/data/site";
import ChapterLabel from "@/components/ui/ChapterLabel";
import CapturesStage from "./effects/CapturesStage";
import Reveal, { Line } from "./effects/Reveal";

const copy = CHAPTERS.captures;

export default function Captures() {
  return (
    <section id="captures" data-family="gain" className="gutter relative py-[12svh]">
      <div className="mx-auto grid max-w-[100rem] gap-y-14 md:grid-cols-12 md:gap-x-10">
        <div className="md:sticky md:top-[14svh] md:col-span-5 md:self-start motion-reduce:static">
          <ChapterLabel index={copy.index}>{copy.kicker}</ChapterLabel>
          <Reveal className="mt-8">
            <h2 className="display text-h2 text-balance">
              <Line>{copy.headline[0]}</Line>
              <Line className="serif-accent">{copy.headline[1]}</Line>
            </h2>
          </Reveal>
          <p className="mt-8 max-w-[34rem] text-lg leading-relaxed text-bone-2">{copy.body}</p>
          <Reveal start="top 88%" stagger={0.07}>
            <ul className="mt-10 max-w-[34rem] border-b border-line">
              {copy.facts.map((fact) => (
                <li key={fact} data-line className="overflow-hidden border-t border-line">
                  <span className="block py-3.5 text-[0.95rem] text-bone">{fact}</span>
                </li>
              ))}
            </ul>
          </Reveal>
        </div>
        <div className="md:col-span-7 md:pt-[6svh]">
          <CapturesStage />
        </div>
      </div>
    </section>
  );
}
