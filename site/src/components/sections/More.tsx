import { CHAPTERS } from "@/data/site";
import { CLIPS } from "@/data/media";
import ChapterLabel from "@/components/ui/ChapterLabel";
import ArriveClip from "./effects/ArriveClip";
import ChainSlots from "./effects/ChainSlots";
import Reveal, { Line } from "./effects/Reveal";

const copy = CHAPTERS.more;
const [plugins, lanes, presets] = copy.items;

function Item({ title, body, index }: { title: string; body: string; index: number }) {
  return (
    <Reveal stagger={0.08}>
      <p data-line className="label overflow-hidden">
        <span className="block text-[var(--accent)]">{String(index).padStart(2, "0")}</span>
      </p>
      <h3 className="mt-4 text-h3 font-semibold leading-tight tracking-tight text-balance">
        <Line>{title}</Line>
      </h3>
      <p className="mt-4 max-w-[30rem] text-lg leading-relaxed text-bone-2">{body}</p>
    </Reveal>
  );
}

export default function More() {
  return (
    <section id="more" className="gutter relative py-[16svh]">
      <div className="mx-auto max-w-[100rem]">
        <ChapterLabel index={copy.index}>{copy.kicker}</ChapterLabel>
        <Reveal className="mt-8">
          <h2 className="display text-h2 text-balance">
            <Line>{copy.headline[0]}</Line>
            <Line className="serif-accent">{copy.headline[1]}</Line>
          </h2>
        </Reveal>

        <div className="mt-[12svh] grid gap-y-[14svh]">
          <article className="grid items-center gap-y-10 md:grid-cols-12 md:gap-x-10">
            <div className="md:col-span-4">
              <Item index={1} {...plugins} />
            </div>
            <div className="md:col-span-8">
              <ChainSlots />
            </div>
          </article>

          <article className="grid items-end gap-y-10 md:grid-cols-12 md:gap-x-10">
            <div className="md:order-2 md:col-span-4 md:pb-[4svh]">
              <Item index={2} {...lanes} />
            </div>
            <ArriveClip clip={CLIPS.split} from="left" className="md:order-1 md:col-span-8" />
          </article>

          <article className="grid items-center gap-y-10 md:grid-cols-12 md:gap-x-10">
            <div className="md:col-span-4 md:col-start-2">
              <Item index={3} {...presets} />
            </div>
            <ArriveClip clip={CLIPS.compare} from="right" className="md:col-span-6 md:col-start-7" />
          </article>
        </div>
      </div>
    </section>
  );
}
