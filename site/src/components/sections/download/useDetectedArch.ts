"use client";

import { useEffect, useState, useSyncExternalStore } from "react";
import { detectArch, detectNotAMac } from "@/lib/platform";
import type { Arch } from "@/lib/release-schema";

// One detection per page: it opens a WebGL context, and the nav and the
// download chapter both ask.
let detection: Promise<Arch | null> | null = null;

/** Null until mounted, and null for a Mac that cannot be told apart. */
export function useDetectedArch(): Arch | null {
  const [arch, setArch] = useState<Arch | null>(null);
  useEffect(() => {
    let live = true;
    detection ??= detectArch().catch(() => null);
    detection.then((a) => live && setArch(a));
    return () => {
      live = false;
    };
  }, []);
  return arch;
}

const never = () => () => {};

/** False on the server and on a Mac: the page is written for the Mac and changes only for a device that is plainly something else. */
export function useNotAMac(): boolean {
  return useSyncExternalStore(never, detectNotAMac, () => false);
}
