/** The small mark each block carries on the board, drawn after the plugin's own. */
const PATHS: Record<string, string> = {
  gate: "M8 5v14M16 5v14M8 12h8",
  comp: "M12 3v5.5M9 5.5 12 8.5l3-3M12 21v-5.5M9 18.5l3-3 3 3M5 12h14",
  drive: "M3 12c2.5 0 3-6 5.5-6s3 12 5.5 12 3-6 5.5-6",
  amp: "M4.5 8.5h15v10h-15zM9 8.5V6h6v2.5M8 14.5h8",
  cab: "M5.5 4h13v16h-13zM15.5 13a3.5 3.5 0 1 1-7 0 3.5 3.5 0 0 1 7 0M12 7.5v.01",
  eq: "M7 4v16M12 4v16M17 4v16M5 9h4M10 15h4M15 7h4",
  mod: "M3 13c1.5 0 2-5 3.5-5s2 9 3.5 9 2-9 3.5-9 2 9 3.5 9 2-4 3.5-4",
  delay: "M6 5v14M10.5 7v10M14.5 9v6M18 10.5v3",
  reverb: "M5 12h.01M9.5 8.5a5 5 0 0 1 0 7M13 6a8.5 8.5 0 0 1 0 12M16.5 3.5a12 12 0 0 1 0 17",
};

export default function Glyph({ id, className = "" }: { id: string; className?: string }) {
  return (
    <svg
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth={1.5}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden
      className={className}
    >
      <path d={PATHS[id]} />
    </svg>
  );
}
