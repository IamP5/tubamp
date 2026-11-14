/**
 * Motion tokens for `motion` transitions — ui/DESIGN.md §Motion, mirroring the
 * CSS tokens `--ds-t-fast/base/slow` and `--ds-ease` in tokens.css.
 *
 * Never inline a raw ms / cubic-bezier / spring config in a component: import
 * from here so the whole app shares one vocabulary.
 */

/** Durations in seconds (Motion's unit). */
export const duration = {
  /** 100ms — a state flip that must never feel laggy (power, A/B). */
  instant: 0.1,
  /** 120ms — hover and press (`--ds-t-fast`). */
  fast: 0.12,
  /** 180ms — selection, expand, reorder settle (`--ds-t-base`). */
  base: 0.18,
  /** 240ms — overlays in and out (`--ds-t-slow`). */
  slow: 0.24,
  /** 420ms — zoom-to-fit, a full preset crossfade. */
  deliberate: 0.42,
} as const;

/** Same values in milliseconds, for timers. */
export const durationMs = {
  instant: 100,
  fast: 120,
  base: 180,
  slow: 240,
  deliberate: 420,
} as const;

/** The one easing (`--ds-ease`). */
const STANDARD = [0.2, 0, 0, 1] as const;

/**
 * Eases for tween transitions. One curve: the names survive so call sites read
 * their intent, but every one of them is `--ds-ease`.
 */
export const ease = {
  standard: STANDARD,
  out: STANDARD,
  inOut: STANDARD,
  in: STANDARD,
  micro: STANDARD,
} as const;

/**
 * Springs for physical properties (x, y, scale, rotate, layout, drag).
 *
 * Invariant: damping / (2 * sqrt(stiffness * mass)) >= 0.85 for every token, so
 * everything settles with no visible bounce. `dampingRatio()` below exists to check new tokens.
 */
export const spring = {
  /** Hover/press micro — settles within ~120ms, zero overshoot. */
  micro: { type: "spring", stiffness: 500, damping: 40, mass: 0.5 },
  /** Default UI spring — select ring, knob snap-back, small chrome. */
  ui: { type: "spring", stiffness: 400, damping: 32, mass: 0.8 },
  /** Drag pick-up (block lift) — livelier, still no bounce (ζ ≈ 0.92). */
  pickup: { type: "spring", stiffness: 380, damping: 36, mass: 1 },
  /** Drop / settle after reorder — no visible bounce (ζ ≈ 0.98). */
  settle: { type: "spring", stiffness: 300, damping: 34, mass: 1 },
  /** Layout reflow of siblings when a block moves / is added / removed. */
  reflow: { type: "spring", stiffness: 350, damping: 34, mass: 1 },
  /** Canvas pan/zoom — heavier, feels like a real surface. */
  canvas: { type: "spring", stiffness: 220, damping: 30, mass: 1.4 },
} as const;

export type SpringToken = keyof typeof spring;

/** ζ = damping / (2·sqrt(stiffness·mass)) — must stay >= 0.85. */
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

/** Stagger tokens. */
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

/** Meter smoothing time constants, in seconds. */
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
