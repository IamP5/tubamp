/**
 * PROTOTYPE — THROWAWAY. TONE3000 in-plugin browser: three structurally
 * different variants over the live app, switchable via `?variant=A|B|C`
 * (prototype skill, sub-shape A). Open http://localhost:5173/?variant=A.
 * Dev builds only; App.tsx mounts this behind import.meta.env.DEV.
 */
import { useEffect, useState } from "react";
import {
  PrototypeSwitcher,
  setVariantParam,
  useVariantParam,
} from "./PrototypeSwitcher";
import { VariantA } from "./VariantA";
import { VariantB } from "./VariantB";
import { VariantC } from "./VariantC";

const VARIANTS = [
  { key: "A", name: "Store takeover" },
  { key: "B", name: "Palette modal" },
  { key: "C", name: "Side drawer" },
];

export function T3kBrowserPrototype() {
  // Re-render on our synthetic popstate so the ?variant= pill stays live.
  const [, bump] = useState(0);
  useEffect(() => {
    const onPop = () => bump((n) => n + 1);
    window.addEventListener("popstate", onPop);
    return () => window.removeEventListener("popstate", onPop);
  }, []);

  const variant = useVariantParam();
  if (variant === null) return null;
  const key = VARIANTS.some((v) => v.key === variant) ? variant : "A";

  return (
    <>
      {key === "A" && <VariantA />}
      {key === "B" && <VariantB />}
      {key === "C" && <VariantC />}
      <PrototypeSwitcher
        variants={VARIANTS}
        current={key}
        onClose={() => setVariantParam(null)}
      />
    </>
  );
}
