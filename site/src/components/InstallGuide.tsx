"use client";

import { useEffect, useState } from "react";
import { AnimatePresence, motion } from "motion/react";
import { CHAPTERS } from "@/data/site";
import { EASE } from "@/lib/motion";
import { installSteps } from "./sections/download/format";

/**
 * A download click is where a visitor is most likely to get lost: the file
 * lands in a folder and the page says nothing. This says what to do next.
 */
export default function InstallGuide({ gatekeeper }: { gatekeeper: boolean }) {
  const [open, setOpen] = useState(false);
  const steps = installSteps(CHAPTERS.download.steps, gatekeeper).slice(1);

  useEffect(() => {
    const onClick = (e: MouseEvent) => {
      const link = (e.target as Element).closest?.("a[href^='/download/']");
      if (link) setOpen(true);
    };
    const onKey = (e: KeyboardEvent) => e.key === "Escape" && setOpen(false);
    document.addEventListener("click", onClick);
    document.addEventListener("keydown", onKey);
    return () => {
      document.removeEventListener("click", onClick);
      document.removeEventListener("keydown", onKey);
    };
  }, []);

  return (
    <AnimatePresence>
      {open && (
        <motion.aside
          role="status"
          initial={{ opacity: 0, y: 24 }}
          animate={{ opacity: 1, y: 0 }}
          exit={{ opacity: 0, y: 16 }}
          transition={{ duration: 0.45, ease: EASE.out }}
          className="fixed inset-x-4 bottom-4 z-50 rounded-2xl border border-line-strong bg-ink-2 p-5 shadow-[0_24px_80px_rgb(0_0_0/0.6)] sm:left-auto sm:w-[22rem]"
        >
          <div className="flex items-start justify-between gap-4">
            <p className="flex items-center gap-2.5 font-medium text-bone">
              <span className="size-1.5 rounded-full bg-signal" aria-hidden />
              Your download has started
            </p>
            <button
              type="button"
              onClick={() => setOpen(false)}
              aria-label="Close"
              className="-m-1.5 cursor-pointer rounded-full p-1.5 text-bone-3 transition-colors duration-150 ease-ui hover:text-bone"
            >
              <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" aria-hidden>
                <path d="M3.5 3.5l9 9M12.5 3.5l-9 9" />
              </svg>
            </button>
          </div>
          <ol className="mt-4 flex flex-col gap-2.5 text-sm text-bone-2">
            {steps.map((step, i) => (
              <li key={step} className="flex gap-3">
                <span className="label w-3 shrink-0 pt-[0.4em] text-signal" aria-hidden>
                  {i + 1}
                </span>
                {step}
              </li>
            ))}
          </ol>
        </motion.aside>
      )}
    </AnimatePresence>
  );
}
