import { FACTS } from "@/data/site";
import Reveal from "./effects/Reveal";

export default function FactStrip() {
  return (
    <section aria-label="At a glance" className="gutter relative border-y border-line bg-ink">
      <Reveal start="top 95%" stagger={0.06}>
        <ul className="mx-auto grid max-w-[110rem] sm:grid-cols-2 lg:grid-cols-4">
          {FACTS.map((fact) => (
            <li
              key={fact.big}
              data-line
              className="overflow-hidden border-line py-7 not-first:border-t sm:px-8 sm:not-first:border-t-0 sm:odd:pl-0 sm:even:border-l sm:nth-[n+3]:border-t lg:border-t-0! lg:px-8 lg:not-first:border-l lg:not-first:pl-8 lg:first:pl-0"
            >
              <div className="flex flex-col gap-2.5">
                <span className="display text-[2rem] text-bone">{fact.big}</span>
                <span className="max-w-[17rem] text-sm leading-snug text-bone-2">{fact.small}</span>
              </div>
            </li>
          ))}
        </ul>
      </Reveal>
    </section>
  );
}
