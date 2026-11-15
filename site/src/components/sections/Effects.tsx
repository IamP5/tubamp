import { getDictionary } from "@/i18n/dictionaries";
import ChapterLabel from "@/components/ui/ChapterLabel";
import EffectsDrum from "./effects/EffectsDrum";

export default async function Effects() {
  const copy = (await getDictionary()).effects;
  const header = (
    <header className="grid gap-y-6 md:grid-cols-12 md:items-end md:gap-x-10">
      <div className="md:col-span-7">
        <ChapterLabel>{copy.kicker}</ChapterLabel>
        <h2 className="display mt-5 text-h2 text-balance">
          <span className="lead-in">{copy.headline[0]}</span> {copy.headline[1]}
        </h2>
      </div>
      <p className="max-w-[34rem] text-base leading-relaxed text-bone-2 md:col-span-5 md:justify-self-end md:text-lg">
        {copy.body}
      </p>
    </header>
  );
  return (
    <section id="effects" aria-label={copy.kicker} className="relative">
      <EffectsDrum header={header} />
    </section>
  );
}
