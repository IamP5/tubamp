"use client";

import ButtonLink from "@/components/ui/Button";
import { useDetectedArch } from "./useDetectedArch";

/** The Download button for the nav and the hero: points at the visitor's Mac once it is known. */
export default function DownloadCta({
  label = "Download for Mac",
  className = "",
}: {
  label?: string;
  className?: string;
}) {
  const arch = useDetectedArch();
  return (
    <ButtonLink href={`/download/${arch ?? "arm64"}`} className={className}>
      {label}
    </ButtonLink>
  );
}
