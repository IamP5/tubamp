"use client";

import { useEffect, useRef, useState } from "react";
import ButtonLink, { buttonClass } from "@/components/ui/Button";
import { SITE } from "@/data/site";
import { useDict } from "@/i18n/DictProvider";
import { ctaHref } from "./format";
import { useDetectedArch, useNotAMac } from "./useDetectedArch";

export default function DownloadCta({
  label,
  shareLabel,
  className = "",
}: {
  label?: string;
  shareLabel?: string;
  className?: string;
}) {
  const arch = useDetectedArch();
  const away = useNotAMac();
  const t = useDict();
  if (away) return <SendLink label={shareLabel ?? t.share.label} className={className} />;
  return (
    <ButtonLink href={ctaHref(arch)} className={className}>
      {label ?? t.nav.downloadMac}
    </ButtonLink>
  );
}

/** A phone cannot install a Mac plugin, so its button gets the page to the Mac instead. */
function SendLink({ label, className }: { label: string; className: string }) {
  const [copied, setCopied] = useState(false);
  const t = useDict();
  const timer = useRef<ReturnType<typeof setTimeout>>(undefined);
  useEffect(() => () => clearTimeout(timer.current), []);

  const send = async () => {
    const url = location.origin + location.pathname;
    if (navigator.share) {
      try {
        await navigator.share({ title: SITE.name, url });
        return;
      } catch (error) {
        // Closing the share sheet is an answer; anything else falls back to the clipboard.
        if (error instanceof DOMException && error.name === "AbortError") return;
      }
    }
    try {
      await navigator.clipboard.writeText(url);
    } catch {
      return;
    }
    setCopied(true);
    clearTimeout(timer.current);
    timer.current = setTimeout(() => setCopied(false), 2000);
  };

  return (
    <button type="button" onClick={send} className={buttonClass("signal", `cursor-pointer ${className}`)}>
      <span aria-live="polite">{copied ? t.share.copied : label}</span>
    </button>
  );
}
