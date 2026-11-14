/** The download buttons on the amber field: the primary is ink on amber, the other an outline. */
export function fieldButton(primary: boolean) {
  const base =
    "group inline-flex h-16 w-full items-center justify-between gap-4 rounded-full px-7 text-lg font-medium transition-[background-color,color,border-color,transform] duration-200 ease-ui active:scale-[0.98]";
  return primary
    ? `${base} bg-on-signal text-signal hover:bg-ink hover:text-bone`
    : `${base} border-2 border-on-signal/70 text-on-signal hover:border-on-signal hover:bg-on-signal hover:text-signal`;
}

const arrow = {
  width: 20,
  height: 20,
  viewBox: "0 0 16 16",
  fill: "none",
  stroke: "currentColor",
  strokeWidth: 1.5,
  strokeLinecap: "round",
  strokeLinejoin: "round",
  "aria-hidden": true,
} as const;

export function ArrowDown() {
  return (
    <svg {...arrow} className="transition-transform duration-200 ease-ui group-hover:translate-y-0.5">
      <path d="M8 2.5v10M3.5 8.5 8 13l4.5-4.5" />
    </svg>
  );
}

/** For links that leave the page rather than start a download. */
export function ArrowOut() {
  return (
    <svg {...arrow} className="transition-transform duration-200 ease-ui group-hover:translate-x-0.5 group-hover:-translate-y-0.5">
      <path d="M4.5 11.5l7-7M5.5 4.5h6v6" />
    </svg>
  );
}
