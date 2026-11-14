import ChapterLabel from "@/components/ui/ChapterLabel";
import VideoLoop from "@/components/ui/VideoLoop";
import Window from "@/components/ui/Window";
import { CLIPS } from "@/data/media";
import { CHAIN, CHAPTERS } from "@/data/site";
import ChainStage from "./chain/ChainStage";
import { SWAP_LINE, pad } from "./chain/copy";

const copy = CHAPTERS.chain;

export default function Chain() {
  const header = (
    <header className="grid gap-6 lg:grid-cols-12 lg:items-end lg:gap-10">
      <div className="lg:col-span-7">
        <div data-rise>
          <ChapterLabel index={copy.index}>{copy.kicker}</ChapterLabel>
        </div>
        <h2 id="chain-title" className="display mt-6 text-h2">
          <span data-rise className="block">
            {copy.headline[0]}
          </span>
          <span data-rise className="serif-accent block">
            {copy.headline[1]}
          </span>
        </h2>
      </div>
      <p data-rise className="max-w-[34rem] text-base text-bone-2 md:text-lg lg:col-span-5 lg:justify-self-end">
        {copy.body}
      </p>
    </header>
  );

  const after = (
    <>
      <ol className="mt-12 hidden gap-x-10 gap-y-4 motion-reduce:grid sm:grid-cols-2 lg:grid-cols-3">
        {CHAIN.map((b, i) => (
          <li key={b.id} className="flex gap-3">
            <span className="label pt-1.5">{pad(i + 1)}</span>
            <p>
              <span className="text-bone">{b.name}.</span> <span className="text-bone-2">{b.does}</span>
            </p>
          </li>
        ))}
        <li className="flex gap-3 sm:col-span-2 lg:col-span-3">
          <p>
            <span className="text-bone">{SWAP_LINE.title}</span> <span className="text-bone-2">{SWAP_LINE.body}</span>
          </p>
        </li>
      </ol>
      <div data-handover className="mt-16 md:mt-24">
        <Window>
          <VideoLoop clip={CLIPS.chainBuild} className="block h-auto w-full" />
        </Window>
      </div>
    </>
  );

  return (
    <section id="chain" aria-labelledby="chain-title" className="gutter relative py-24 md:py-32">
      <ChainStage header={header} after={after} />
    </section>
  );
}
