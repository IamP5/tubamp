import Footer from "@/components/Footer";
import HeroA from "@/components/hero/a";
import Nav from "@/components/Nav";
import Captures from "@/components/sections/Captures";
import Chain from "@/components/sections/Chain";
import Demo from "@/components/sections/Demo";
import Download from "@/components/sections/Download";
import Effects from "@/components/sections/Effects";
import Marquee from "@/components/sections/Marquee";
import More from "@/components/sections/More";
import { getLatestRelease } from "@/lib/release";

export default async function Home() {
  const release = await getLatestRelease();
  return (
    <>
      <Nav />
      <main>
        <HeroA version={release?.version ?? null} />
        <Marquee />
        <Chain />
        <Captures />
        <Effects />
        <More />
        <Demo />
        <Download />
      </main>
      <Footer />
    </>
  );
}
