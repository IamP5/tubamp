import { getDictionary } from "@/i18n/dictionaries";
import { CLIPS } from "@/data/media";
import ChapterLabel from "@/components/ui/ChapterLabel";
import ArriveClip from "./effects/ArriveClip";
import ChainSlots from "./effects/ChainSlots";
import Reveal, { Line } from "./effects/Reveal";

function Item({ title, body, index, children }: { title: string; body: string; index: number; children: React.ReactNode }) {
  return (
    <article className="flex flex-col">
      <div className="flex items-center lg:h-[17rem]">
        <div className="w-full">{children}</div>
      </div>
      <Reveal stagger={0.08} className="mt-8">
        <p data-line className="label overflow-hidden">
          <span className="block text-[var(--accent)]">{String(index).padStart(2, "0")}</span>
        </p>
        <h3 className="mt-3 text-2xl font-semibold leading-tight tracking-tight text-balance">
          <Line>{title}</Line>
        </h3>
        <p className="mt-3 max-w-[30rem] leading-relaxed text-bone-2">{body}</p>
      </Reveal>
    </article>
  );
}

export default async function More() {
  const copy = (await getDictionary()).more;
  const [plugins, lanes, presets] = copy.items;
  return (
    <section id="more" className="gutter relative py-[9svh]">
      <div className="mx-auto max-w-[110rem]">
        <ChapterLabel>{copy.kicker}</ChapterLabel>
        <Reveal className="mt-6">
          <h2 className="display text-h2 text-balance">
            <Line className="lead-in">{copy.headline[0]}</Line>
            <Line>{copy.headline[1]}</Line>
          </h2>
        </Reveal>

        <div className="mt-[6svh] grid gap-x-10 gap-y-16 lg:grid-cols-[1.3fr_1fr_1fr]">
          <Item index={1} {...plugins}>
            <ChainSlots />
          </Item>
          <Item index={2} {...lanes}>
            <ArriveClip clip={CLIPS.split} from="center" />
          </Item>
          <Item index={3} {...presets}>
            <ArriveClip clip={CLIPS.compare} from="center" />
          </Item>
        </div>
      </div>
    </section>
  );
}
