import Footer from "@/components/Footer";
import HeroA from "@/components/hero/a";
import Nav from "@/components/Nav";
import Captures from "@/components/sections/Captures";
import Chain from "@/components/sections/Chain";
import Demo from "@/components/sections/Demo";
import Download from "@/components/sections/Download";
import Effects from "@/components/sections/Effects";
import Interlude from "@/components/sections/Interlude";
import Marquee from "@/components/sections/Marquee";
import More from "@/components/sections/More";
import { FILMS } from "@/data/media";
import { INTERLUDES } from "@/data/site";
import { getLatestRelease } from "@/lib/release";

export default async function Home() {
  const release = await getLatestRelease();
  return (
    <>
      <Nav />
      <main>
        <HeroA version={release?.version ?? null} />
        <Marquee />
        <Interlude film={FILMS.strings} opening="blinds" {...INTERLUDES.strings} />
        <Chain />
        <Captures />
        <Effects />
        <More />
        <Demo />
        <Interlude film={FILMS.speaker} opening="grid" {...INTERLUDES.speaker}>
          <Download />
        </Interlude>
      </main>
      <Footer />
    </>
  );
}
