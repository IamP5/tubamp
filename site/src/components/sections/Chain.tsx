import ChapterLabel from "@/components/ui/ChapterLabel";
import { CHAPTERS } from "@/data/site";
import ChainBoard from "./chain/ChainBoard";
import Reveal, { Line } from "./effects/Reveal";

const copy = CHAPTERS.chain;

export default function Chain() {
  return (
    <section id="chain" aria-labelledby="chain-title" className="gutter relative py-20 md:py-[9svh]">
      <div className="mx-auto max-w-[110rem]">
        <header className="grid gap-6 lg:grid-cols-12 lg:items-end lg:gap-10">
          <div className="lg:col-span-7">
            <ChapterLabel>{copy.kicker}</ChapterLabel>
            <Reveal className="mt-6">
              <h2 id="chain-title" className="display text-h2">
                <Line className="lead-in">{copy.headline[0]}</Line>
                <Line>{copy.headline[1]}</Line>
              </h2>
            </Reveal>
          </div>
          <p className="max-w-[34rem] text-base text-bone-2 md:text-lg lg:col-span-5 lg:justify-self-end">{copy.body}</p>
        </header>
        <ChainBoard />
      </div>
    </section>
  );
}
