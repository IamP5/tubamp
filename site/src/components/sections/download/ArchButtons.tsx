"use client";

import { useEffect, useRef, useState } from "react";
import { CHAPTERS } from "@/data/site";
import { formatSize, type Arch, type Download } from "@/lib/release-schema";
import { archOrder, shortHash } from "./format";
import { ArrowDown, fieldButton } from "./parts";
import { useDetectedArch } from "./useDetectedArch";

const { archLabels } = CHAPTERS.download;

export default function ArchButtons({ downloads }: { downloads: Record<Arch, Download> }) {
  const detected = useDetectedArch();
  const order = archOrder(detected);
  return (
    <ul className="grid gap-8 sm:grid-cols-2 sm:gap-4">
      {order.map((arch, i) => (
        <li key={arch} className="flex flex-col gap-3">
          <a href={`/download/${arch}`} className={fieldButton(detected !== null && i === 0)}>
            <span>
              <span className="sr-only">Download for </span>
              {archLabels[arch]}
            </span>
            <ArrowDown />
          </a>
          <p className="flex flex-wrap items-center gap-x-3 gap-y-1 pl-1 font-mono text-xs tabular-nums text-bone-3">
            <span>{formatSize(downloads[arch].size)}</span>
            <span aria-hidden className="h-3 w-px bg-line-strong" />
            <span>
              <span className="text-bone-3">SHA-256 </span>
              {shortHash(downloads[arch].sha256)}
            </span>
            <CopyHash sha256={downloads[arch].sha256} label={archLabels[arch]} />
          </p>
        </li>
      ))}
    </ul>
  );
}

function CopyHash({ sha256, label }: { sha256: string; label: string }) {
  const [copied, setCopied] = useState(false);
  const timer = useRef<ReturnType<typeof setTimeout>>(undefined);
  useEffect(() => () => clearTimeout(timer.current), []);

  const copy = async () => {
    try {
      await navigator.clipboard.writeText(sha256);
    } catch {
      return;
    }
    setCopied(true);
    clearTimeout(timer.current);
    timer.current = setTimeout(() => setCopied(false), 1600);
  };

  return (
    <button
      type="button"
      onClick={copy}
      className="-my-1 inline-flex items-center gap-1.5 rounded-full px-2 py-1 text-bone-2 transition-colors duration-150 ease-ui hover:bg-ink-3 hover:text-bone"
      aria-label={`Copy the full SHA-256 of the ${label} installer`}
    >
      {copied ? <Check /> : <CopyIcon />}
      <span aria-live="polite">{copied ? "Copied" : "Copy"}</span>
    </button>
  );
}

const icon = {
  width: 14,
  height: 14,
  viewBox: "0 0 16 16",
  fill: "none",
  stroke: "currentColor",
  strokeWidth: 1.5,
  strokeLinecap: "round",
  strokeLinejoin: "round",
  "aria-hidden": true,
} as const;

function CopyIcon() {
  return (
    <svg {...icon}>
      <rect x="5.5" y="5.5" width="8" height="8" rx="1.5" />
      <path d="M10.5 3.5v-.5A1.5 1.5 0 0 0 9 1.5H3A1.5 1.5 0 0 0 1.5 3v6A1.5 1.5 0 0 0 3 10.5h.5" />
    </svg>
  );
}

function Check() {
  return (
    <svg {...icon}>
      <path d="M3 8.5l3 3 7-7" />
    </svg>
  );
}
