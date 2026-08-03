/**
 * The generic knob row shared by gate / comp / drive / cab / eq / mod / delay /
 * reverb. Cells are fixed-width and centred, like the native `layoutKnobRow`.
 *
 * Knob accents come from the `[data-block]` attribute on the panel root, so no
 * per-knob colour prop is needed.
 */
import { Knob } from "../../components";
import type { KnobSpec } from "./knobSpecs";
import s from "./panel.module.css";

export interface KnobRowProps {
  specs: readonly KnobSpec[];
  /** Smaller dials for rows that share vertical space with another body. */
  size?: "sm" | "md";
  className?: string;
}

export function KnobRow({ specs, size = "md", className }: KnobRowProps) {
  if (specs.length === 0) return null;
  return (
    <div className={className ? `${s.knobRow} ${className}` : s.knobRow}>
      {specs.map((spec) => (
        <div key={spec.id} className={s.knobCell}>
          <Knob id={spec.id} label={spec.label} size={size} />
        </div>
      ))}
    </div>
  );
}
