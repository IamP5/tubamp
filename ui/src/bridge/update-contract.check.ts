/**
 * Holds update-contract.json to the types in ./types.ts. A JSON import widens every
 * string to `string`, so the literal kinds, reasons and command types are checked as
 * the golden file's keys, and C++ (`tubamp_update_tool test`) checks that each value's
 * own kind, reason or type equals its key. Nothing imports this file; `tsc` checks it.
 */
import type golden from "./update-contract.json";
import type {
  UpdateCheckProblem,
  UpdateCommand,
  UpdateFailure,
  UpdateManualReason,
  UpdatePhase,
  UpdateView,
} from "./types";

type G = typeof golden;
type Kind = UpdatePhase["kind"];
type PhaseOf<K> = Extract<UpdatePhase, { kind: K }>;
type CommandOf<T> = Extract<UpdateCommand, { type: T }>;

type Same<A, B> = [A] extends [B] ? ([B] extends [A] ? true : false) : false;
type Expect<T extends true> = T;
type AllKeys<T> = T extends unknown ? keyof T : never;
type Widen<T> = T extends string
  ? string
  : T extends number
    ? number
    : T extends boolean
      ? boolean
      : T extends object
        ? { [K in keyof T]: Widen<T[K]> }
        : T;
/** V carries every required field of T with the right type, and no field T lacks. */
type Fits<V, T> = [V] extends [Widen<T>] ? Same<Exclude<keyof V, AllKeys<T>>, never> : false;
type All<M> = Same<M[keyof M], true>;

type Views = { [K in Kind]: [Fits<G["views"][K], UpdateView>, Fits<G["views"][K]["phase"], PhaseOf<K>>] };
type Failures = { [R in UpdateFailure]: Fits<G["failures"][R], PhaseOf<"failed">> };
type Reasons = { [R in UpdateManualReason]: Fits<G["manualReasons"][R], PhaseOf<"manual">> };
type Problems = { [P in UpdateCheckProblem]: Fits<G["checkProblems"][P], PhaseOf<"idle">> };
type Commands = { [T in UpdateCommand["type"]]: Fits<G["commands"][T], CommandOf<T>> };

export type UpdateContractHolds = [
  Expect<Same<keyof G["views"], Kind>>,
  Expect<Same<Views[Kind], [true, true]>>,
  Expect<Same<keyof G["failures"], UpdateFailure>>,
  Expect<All<Failures>>,
  Expect<Same<keyof G["manualReasons"], UpdateManualReason>>,
  Expect<All<Reasons>>,
  Expect<Same<keyof G["checkProblems"], UpdateCheckProblem>>,
  Expect<All<Problems>>,
  Expect<Same<keyof G["commands"], UpdateCommand["type"]>>,
  Expect<All<Commands>>,
];
