import Footer from "@/components/Footer";
import Hero from "@/components/hero";
import InstallGuide from "@/components/InstallGuide";
import Nav from "@/components/Nav";
import Captures from "@/components/sections/Captures";
import Chain from "@/components/sections/Chain";
import Demo from "@/components/sections/Demo";
import Download from "@/components/sections/Download";
import Effects from "@/components/sections/Effects";
import FactStrip from "@/components/sections/FactStrip";
import More from "@/components/sections/More";
import { unsigned } from "@/components/sections/download/format";
import { getLatestRelease } from "@/lib/release";

export default async function Home() {
  const release = await getLatestRelease();
  return (
    <>
      <Nav />
      <main>
        <Hero release={release} />
        <FactStrip />
        <Chain />
        <Captures />
        <Effects />
        <Demo />
        <More />
        <Download />
      </main>
      <Footer />
      <InstallGuide gatekeeper={unsigned(release)} />
    </>
  );
}
