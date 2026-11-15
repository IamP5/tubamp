"use client";

import { useEffect, useState } from "react";
import { NAV, SITE } from "@/data/site";
import { useDict } from "@/i18n/DictProvider";
import DownloadCta from "./sections/download/DownloadCta";

function useScrolled(): boolean {
  const [scrolled, setScrolled] = useState(false);
  useEffect(() => {
    const read = () => setScrolled(window.scrollY > 8);
    read();
    window.addEventListener("scroll", read, { passive: true });
    return () => window.removeEventListener("scroll", read);
  }, []);
  return scrolled;
}

/** The bar never hides: the Download button is the page's one job and stays in reach. */
export default function Nav() {
  const scrolled = useScrolled();
  const t = useDict();

  return (
    <header className="fixed inset-x-0 top-0 z-50">
      <div
        aria-hidden
        className={`absolute inset-0 border-b border-line bg-ink/92 transition-opacity duration-300 ease-ui ${
          scrolled ? "opacity-100" : "opacity-0"
        }`}
      />
      <nav aria-label={t.nav.label} className="gutter relative mx-auto flex h-16 max-w-[110rem] items-center justify-between gap-6">
        <a href="#top" className="text-xl font-semibold tracking-[-0.04em] text-bone">
          {SITE.name}
        </a>
        <ul className="hidden items-center gap-8 md:flex">
          {NAV.map((item) => (
            <li key={item.href}>
              <a
                href={item.href}
                className="py-2 text-sm text-bone-2 transition-colors duration-200 ease-ui hover:text-bone"
              >
                {t.nav.items[item.key]}
              </a>
            </li>
          ))}
        </ul>
        <DownloadCta
          label={t.nav.download}
          shareLabel={t.share.short}
          className={`h-10! px-5! text-sm! ${scrolled ? "" : "bg-transparent! text-bone! ring-1 ring-line-strong ring-inset hover:bg-bone! hover:text-ink!"}`}
        />
      </nav>
    </header>
  );
}
