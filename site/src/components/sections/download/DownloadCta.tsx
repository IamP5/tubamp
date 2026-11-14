"use client";

import ButtonLink from "@/components/ui/Button";
import { ctaHref } from "./format";
import { useDetectedArch } from "./useDetectedArch";

export default function DownloadCta({
  label = "Download for Mac",
  className = "",
}: {
  label?: string;
  className?: string;
}) {
  const arch = useDetectedArch();
  return (
    <ButtonLink href={ctaHref(arch)} className={className}>
      {label}
    </ButtonLink>
  );
}
