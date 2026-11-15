"use client";

import { createContext, use } from "react";
import type { Dictionary } from "./en";

const DictContext = createContext<Dictionary | null>(null);

/** Hands the page's strings to Client Components. The root layout renders it once. */
export default function DictProvider({ dict, children }: { dict: Dictionary; children: React.ReactNode }) {
  return <DictContext value={dict}>{children}</DictContext>;
}

export function useDict(): Dictionary {
  const dict = use(DictContext);
  if (!dict) throw new Error("useDict needs a DictProvider above it");
  return dict;
}
