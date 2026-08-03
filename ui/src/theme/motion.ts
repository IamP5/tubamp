/**
 * Motion tokens — implements docs/research/motion-design.md §1.
 *
 * Never inline a raw ms / cubic-bezier / spring config in a component: import
 * from here so the whole app shares one physical vocabulary.
 */

/** Durations in seconds (Motion's unit). See §1.1 for the ms table. */
export const duration = {
  /** 100ms — power dim, LED toggle, colour swaps, meter-peak flash. */
  instant: 0.1,
  /** 150ms — hover/press feedback, tooltip, connector highlight. */
  fast: 0.15,
  /** 220ms — panel content morph, param-panel swap, block select ring. */
  base: 0.22,
  /** 320ms — modal/overlay open, entrance stagger step, A/B flip. */
  slow: 0.32,
  /** 420ms — zoom-to-fit, full preset crossfade. */
  deliberate: 0.42,
} as const;

/** Same values in milliseconds, for CSS `transition` strings and timers. */
export const durationMs = {
  instant: 100,
  fast: 150,
  base: 220,
  slow: 320,
  deliberate: 420,
} as const;

/** Cubic-bezier eases for tween transitions (opacity / colour / non-physical). */
export const ease = {
  /** Snappy start, soft landing. Default for anything entering or growing. */
  out: [0.16, 1, 0.3, 1] as const,
  /** Symmetric — crossfades / colour swaps with no directional intent. */
  inOut: [0.65, 0, 0.35, 1] as const,
  /** Sharp in — exits and dismissals, gone fast. */
  in: [0.7, 0, 0.84, 0] as const,
  /** Near-linear — hover/press tweens where springs aren't used. */
  micro: [0.4, 0, 0.2, 1] as const,
} as const;

/**
 * Springs for physical properties (x, y, scale, rotate, layout, drag).
 *
 * Invariant: damping / (2 * sqrt(stiffness * mass)) >= 0.75 for every token, so
 * everything is critically-to-slightly-underdamped — one settle tick at most,
 * never a visible bounce. `dampingRatio()` below exists to check new tokens.
 */
export const spring = {
  /** Hover/press micro — settles within ~120ms, zero overshoot. */
  micro: { type: "spring", stiffness: 500, damping: 40, mass: 0.5 },
  /** Default UI spring — select ring, knob snap-back, small chrome. */
  ui: { type: "spring", stiffness: 400, damping: 32, mass: 0.8 },
  /** Drag pick-up (block lift) — livelier, still no bounce. */
  pickup: { type: "spring", stiffness: 380, damping: 28, mass: 1 },
  /** Drop / settle after reorder — the one intentional settle tick (ζ ≈ 0.75). */
  settle: { type: "spring", stiffness: 300, damping: 26, mass: 1 },
  /** Layout reflow of siblings when a block moves / is added / removed. */
  reflow: { type: "spring", stiffness: 350, damping: 34, mass: 1 },
  /** Canvas pan/zoom — heavier, feels like a real surface. */
  canvas: { type: "spring", stiffness: 220, damping: 30, mass: 1.4 },
} as const;

export type SpringToken = keyof typeof spring;

/** ζ = damping / (2·sqrt(stiffness·mass)) — must stay >= 0.75. */
export function dampingRatio(config: {
  stiffness: number;
  damping: number;
  mass: number;
}): number {
  return config.damping / (2 * Math.sqrt(config.stiffness * config.mass));
}

/** Ready-made tween transitions for the common cases. */
export const tween = {
  enter: { duration: duration.base, ease: ease.out },
  exit: { duration: duration.fast, ease: ease.in },
  crossfade: { duration: duration.base, ease: ease.inOut },
  micro: { duration: duration.fast, ease: ease.micro },
  instant: { duration: duration.instant, ease: ease.micro },
} as const;

/** Stagger tokens (§1.4). */
export const stagger = {
  board: { staggerChildren: 0.035, delayChildren: 0.08 },
  list: { staggerChildren: 0.02 },
} as const;

/**
 * Cap total stagger spread at ~150ms regardless of item count, so a 40-item
 * grid never takes longer to reveal than a 6-item one.
 */
export function staggerFor(itemCount: number, spread = 0.15): number {
  if (itemCount <= 1) return 0;
  return Math.min(0.02, spread / itemCount);
}

/** Meter smoothing time constants, in seconds (§2.12). */
export const meterSmoothing = {
  /** ~20ms — near-instant response to rising levels. */
  attackTau: 0.02,
  /** ~350ms — readable decay on falling levels. */
  releaseTau: 0.35,
  /** Peak-hold dwell before the tick starts falling. */
  holdMs: 1500,
  /** Peak-hold fall, in normalised units per second, after the dwell. */
  holdFallPerSecond: 0.45,
} as const;

/**
 * Exponential smoothing coefficient for a time constant `tau` over `dt`
 * seconds — frame-rate independent, unlike the old per-tick 0.75 decay.
 */
export function smoothingCoef(dt: number, tau: number): number {
  return 1 - Math.exp(-dt / tau);
}

/** Motion-library-shaped transition that respects a reduced-motion preference. */
export function reduced<T>(prefersReduced: boolean, motionful: T, plain: T): T {
  return prefersReduced ? plain : motionful;
}

/** Zero-duration transition used as the reduced-motion fallback. */
export const noMotion = { duration: 0 } as const;
