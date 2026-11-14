"use client";

import { useEffect, useState } from "react";
import { NAV, SITE } from "@/data/site";
import DownloadCta from "./sections/download/DownloadCta";

type Bar = { scrolled: boolean; hidden: boolean };

// Past this many pixels the bar is allowed to hide; a small reversal does not bring it back.
const HIDE_AFTER = 120;
const DEADBAND = 6;

function useBar(): Bar {
  const [bar, setBar] = useState<Bar>({ scrolled: false, hidden: false });

  useEffect(() => {
    let last = window.scrollY;
    let frame = 0;
    const read = () => {
      frame = 0;
      const y = window.scrollY;
      const delta = y - last;
      if (Math.abs(delta) < DEADBAND && y > 0) return;
      last = y;
      setBar((prev) => {
        const next = { scrolled: y > 8, hidden: y > HIDE_AFTER ? delta > 0 : false };
        return next.scrolled === prev.scrolled && next.hidden === prev.hidden ? prev : next;
      });
    };
    const onScroll = () => {
      frame ||= requestAnimationFrame(read);
    };
    frame = requestAnimationFrame(read);
    window.addEventListener("scroll", onScroll, { passive: true });
    return () => {
      window.removeEventListener("scroll", onScroll);
      cancelAnimationFrame(frame);
    };
  }, []);

  return bar;
}

export default function Nav() {
  const { scrolled, hidden } = useBar();

  return (
    <header
      className={`fixed inset-x-0 top-0 z-50 transition-transform focus-within:translate-y-0 motion-reduce:transition-none ${
        hidden ? "-translate-y-full duration-300 ease-in" : "translate-y-0 duration-500 ease-out"
      }`}
    >
      <div
        aria-hidden
        className={`absolute inset-0 border-b border-line bg-ink transition-opacity duration-300 ease-ui ${
          scrolled ? "opacity-100" : "opacity-0"
        }`}
      />
      <nav aria-label="Main" className="gutter relative flex h-16 items-center justify-between gap-6 md:h-[4.5rem]">
        <a href="#top" className="text-xl font-semibold tracking-[-0.04em] text-bone">
          {SITE.name}
        </a>
        <ul className="hidden items-center gap-8 md:flex">
          {NAV.map((item) => (
            <li key={item.href}>
              <a
                href={item.href}
                className="group relative py-2 text-sm text-bone-2 transition-colors duration-200 ease-ui hover:text-bone"
              >
                {item.label}
                <span
                  aria-hidden
                  className="absolute inset-x-0 bottom-0.5 h-px origin-left scale-x-0 bg-current transition-transform duration-300 ease-out group-hover:scale-x-100"
                />
              </a>
            </li>
          ))}
        </ul>
        <DownloadCta label="Download" className="h-10! px-5! text-sm!" />
      </nav>
    </header>
  );
}
