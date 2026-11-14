import type { Sequence } from "../../../data/media.ts";

/** Coarse to fine: every 16th frame, then every 8th, and so on, so the film can be scrubbed long before it has all arrived. */
export function loadOrder(count: number): number[] {
  const order: number[] = [];
  const seen = new Set<number>();
  for (let step = 16; step >= 1; step /= 2) {
    for (let i = 0; i < count; i += step) {
      if (!seen.has(i)) {
        seen.add(i);
        order.push(i);
      }
    }
  }
  return order;
}

/** The tier whose frames need the least stretching to fill a box this tall, in device pixels. */
export function pickTier(seq: Sequence, boxHeight: number, constrained: boolean) {
  const [small] = seq.tiers;
  if (constrained) return small;
  return seq.tiers.find((tier) => tier.height >= boxHeight * 0.75) ?? seq.tiers[seq.tiers.length - 1];
}
