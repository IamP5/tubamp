import type { Arch } from "./release-schema";

export type ArchHints = {
  platform: string;
  uaArchitecture?: string;
  glRenderer?: string;
};

/**
 * Browsers on Apple Silicon still report an Intel user agent, so this reads the
 * client-hint architecture where it exists and the GPU name otherwise. Null means
 * "a Mac we cannot tell apart" or "not a Mac".
 */
export function archFromHints({ platform, uaArchitecture, glRenderer }: ArchHints): Arch | null {
  if (!/mac/i.test(platform)) return null;
  if (uaArchitecture === "arm") return "arm64";
  if (uaArchitecture === "x86") return "x86_64";
  if (glRenderer) {
    if (/Apple M\d/i.test(glRenderer)) return "arm64";
    if (/Intel|AMD|Radeon|NVIDIA/i.test(glRenderer)) return "x86_64";
  }
  return null;
}

/**
 * True only for a device that plainly cannot install a Mac plugin. An unknown
 * platform is not a "no". An iPad calls itself a Mac, but no Mac has a touch
 * screen.
 */
export function notAMac({ platform, maxTouchPoints }: { platform: string; maxTouchPoints: number }): boolean {
  if (!platform) return false;
  return /mac/i.test(platform) ? maxTouchPoints > 1 : true;
}

type UADataLike = {
  platform?: string;
  getHighEntropyValues?: (hints: string[]) => Promise<{ architecture?: string }>;
};

function platformName(): string {
  const nav = navigator as Navigator & { userAgentData?: UADataLike };
  return nav.userAgentData?.platform ?? nav.platform ?? "";
}

export function detectNotAMac(): boolean {
  return notAMac({ platform: platformName(), maxTouchPoints: navigator.maxTouchPoints ?? 0 });
}

export async function detectArch(): Promise<Arch | null> {
  const nav = navigator as Navigator & { userAgentData?: UADataLike };
  const platform = platformName();
  let uaArchitecture: string | undefined;
  try {
    uaArchitecture = (await nav.userAgentData?.getHighEntropyValues?.(["architecture"]))?.architecture;
  } catch {
    uaArchitecture = undefined;
  }
  let glRenderer: string | undefined;
  try {
    const gl = document.createElement("canvas").getContext("webgl");
    const ext = gl?.getExtension("WEBGL_debug_renderer_info");
    if (gl && ext) glRenderer = String(gl.getParameter(ext.UNMASKED_RENDERER_WEBGL));
  } catch {
    glRenderer = undefined;
  }
  return archFromHints({ platform, uaArchitecture, glRenderer });
}
