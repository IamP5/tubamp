import type { ComponentProps } from "react";

type Variant = "signal" | "bone" | "ghost";

const base =
  "inline-flex h-12 items-center justify-center gap-2 rounded-full px-6 text-[0.95rem] font-medium transition-[background-color,color,border-color,transform] duration-200 ease-ui active:scale-[0.98]";

const variants: Record<Variant, string> = {
  signal: "bg-signal text-on-signal hover:bg-bone",
  bone: "bg-bone text-ink hover:bg-signal hover:text-on-signal",
  ghost: "border border-line-strong text-bone hover:border-bone",
};

export function buttonClass(variant: Variant = "signal", className = "") {
  return `${base} ${variants[variant]} ${className}`;
}

export default function ButtonLink({
  variant = "signal",
  className = "",
  ...props
}: ComponentProps<"a"> & { variant?: Variant }) {
  return <a className={buttonClass(variant, className)} {...props} />;
}
