/**
 * PROTOTYPE — THROWAWAY. Floating variant switcher (prototype skill §4).
 * High-contrast pill pinned bottom-centre, deliberately alien to the design
 * under evaluation. Dev builds only.
 */
import { useCallback, useEffect } from "react";

export interface VariantDef {
  key: string;
  name: string;
}

export function useVariantParam(): string | null {
  // No router in this SPA — read the search param directly; replaceState keeps
  // it shareable/reload-stable without adding a routing dependency.
  return new URLSearchParams(window.location.search).get("variant");
}

export function setVariantParam(key: string | null): void {
  const url = new URL(window.location.href);
  if (key === null) url.searchParams.delete("variant");
  else url.searchParams.set("variant", key);
  window.history.replaceState(null, "", url.toString());
  // No router → nothing re-renders on replaceState; nudge listeners manually.
  window.dispatchEvent(new PopStateEvent("popstate"));
}

const barStyle: React.CSSProperties = {
  position: "fixed",
  bottom: 10,
  left: "50%",
  transform: "translateX(-50%)",
  zIndex: 9999,
  display: "flex",
  alignItems: "center",
  gap: 10,
  padding: "6px 10px",
  borderRadius: 999,
  background: "#fff",
  color: "#111",
  font: "600 12px/1 -apple-system, sans-serif",
  boxShadow: "0 4px 20px rgba(0,0,0,.6), 0 0 0 1px rgba(0,0,0,.2)",
};

const btnStyle: React.CSSProperties = {
  all: "unset",
  cursor: "pointer",
  padding: "4px 8px",
  borderRadius: 999,
  background: "#111",
  color: "#fff",
  fontWeight: 700,
};

export function PrototypeSwitcher({
  variants,
  current,
  onClose,
}: {
  variants: VariantDef[];
  current: string;
  onClose: () => void;
}) {
  const index = Math.max(0, variants.findIndex((v) => v.key === current));

  const cycle = useCallback(
    (delta: number) => {
      const next = variants[(index + delta + variants.length) % variants.length];
      setVariantParam(next.key);
    },
    [index, variants],
  );

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement | null;
      if (
        target &&
        (target.tagName === "INPUT" ||
          target.tagName === "TEXTAREA" ||
          target.isContentEditable)
      )
        return;
      if (e.key === "ArrowLeft") cycle(-1);
      else if (e.key === "ArrowRight") cycle(1);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [cycle]);

  if (import.meta.env.PROD) return null;

  const def = variants[index];
  return (
    <div style={barStyle}>
      <button style={btnStyle} onClick={() => cycle(-1)} aria-label="Previous variant">
        ←
      </button>
      <span style={{ minWidth: 180, textAlign: "center" }}>
        {def.key} — {def.name}
      </span>
      <button style={btnStyle} onClick={() => cycle(1)} aria-label="Next variant">
        →
      </button>
      <button
        style={{ ...btnStyle, background: "#e5484d" }}
        onClick={onClose}
        aria-label="Close prototype"
      >
        ✕
      </button>
    </div>
  );
}
