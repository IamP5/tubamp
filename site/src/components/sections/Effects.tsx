import { CHAPTERS } from "@/data/site";
import ChapterLabel from "@/components/ui/ChapterLabel";
import EffectsStack from "./effects/EffectsStack";
import Reveal, { Line } from "./effects/Reveal";

const copy = CHAPTERS.effects;
const cut = copy.headline[1].lastIndexOf(" ");
const lead = copy.headline[1].slice(0, cut);
const turn = copy.headline[1].slice(cut + 1);

export default function Effects() {
  return (
    <section id="effects" className="relative">
      <header className="gutter mx-auto grid max-w-[100rem] gap-y-8 pt-[10svh] pb-[4svh] md:pt-[16svh] md:grid-cols-12 md:gap-x-10 md:pb-[12svh]">
        <div className="md:col-span-12">
          <ChapterLabel index={copy.index}>{copy.kicker}</ChapterLabel>
          <Reveal className="mt-8">
            <h2 className="display text-h2 text-balance">
              <Line>{copy.headline[0]}</Line>
              <Line>
                {lead} <span className="serif-accent">{turn}</span>
              </Line>
            </h2>
          </Reveal>
        </div>
        <p className="max-w-[34rem] text-lg leading-relaxed text-bone-2 md:col-span-5 md:col-start-8">{copy.body}</p>
      </header>
      <EffectsStack />
    </section>
  );
}
