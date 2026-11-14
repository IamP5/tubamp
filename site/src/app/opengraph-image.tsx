import { ImageResponse } from "next/og";
import { SITE } from "@/data/site";

export const alt = `${SITE.name}. ${SITE.tagline}`;
export const size = { width: 1200, height: 630 };
export const contentType = "image/png";

// Satori cannot read CSS variables; these are the ink, bone, bone-2 and signal tokens from globals.css.
const INK = "#0b0b0c";
const BONE = "#ededee";
const BONE_2 = "#a3a3a9";
const SIGNAL = "#f0a63a";

const wave = Array.from({ length: 61 }, (_, i) => {
  const x = i * 20;
  const y = 40 + Math.sin(i / 3.2) * 26 * Math.min(1, i / 18);
  return `${i === 0 ? "M" : "L"}${x} ${y.toFixed(1)}`;
}).join(" ");

export default function Image() {
  return new ImageResponse(
    (
      <div
        style={{
          width: "100%",
          height: "100%",
          display: "flex",
          flexDirection: "column",
          justifyContent: "space-between",
          background: INK,
          color: BONE,
          padding: "64px 72px 0",
        }}
      >
        <div style={{ display: "flex", fontSize: 34, letterSpacing: "-0.04em", color: BONE_2 }}>{SITE.name}</div>
        <div style={{ display: "flex", flexDirection: "column", fontSize: 124, lineHeight: 0.92, letterSpacing: "-0.05em" }}>
          <span>Your next amp</span>
          <span>is a file.</span>
        </div>
        <svg width="1200" height="80" viewBox="0 0 1200 80" style={{ marginLeft: -72, marginBottom: 36 }}>
          <path d={wave} fill="none" stroke={SIGNAL} strokeWidth="4" strokeLinecap="round" strokeLinejoin="round" />
        </svg>
      </div>
    ),
    size,
  );
}
