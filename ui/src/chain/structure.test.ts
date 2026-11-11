/**
 * Self-checking tests for ./structure.ts — no runner dependency on purpose (the
 * UI has none). Run from ui/:
 *
 *     node_modules/.bin/esbuild src/chain/structure.test.ts --bundle \
 *       --platform=node --log-level=warning | node
 *
 * Exits non-zero on the first failure. Every helper that produces an order is
 * also checked to be a fixed point of `sanitizeStructure`: that is the contract
 * that lets the board publish its result in one write.
 */
import type { BlockId } from "../bridge/types";
import {
  MAX_CHAIN_LENGTH,
  blocksDroppedBy,
  dualAmpOrder,
  findStructure,
  flattenStructure,
  insertSplit,
  removeSplit,
  sanitizeStructure,
  swapLanes,
} from "./structure";

let failures = 0;
let passes = 0;

function same(a: readonly unknown[], b: readonly unknown[]): boolean {
  return a.length === b.length && a.every((v, i) => v === b[i]);
}

function eq(name: string, got: readonly unknown[], want: readonly unknown[]): void {
  if (same(got, want)) {
    passes += 1;
    return;
  }
  failures += 1;
  console.error(`FAIL ${name}\n  got:  ${got.join(",")}\n  want: ${want.join(",")}`);
}

function ok(name: string, cond: boolean): void {
  if (cond) passes += 1;
  else {
    failures += 1;
    console.error(`FAIL ${name}`);
  }
}

function valid(name: string, order: readonly BlockId[]): void {
  eq(`${name} is sanitize-stable`, sanitizeStructure(order), order);
}

const rig: BlockId[] = ["gate", "split", "amp", "eq", "lane2", "amp2", "mix", "reverb"];

/* removeSplit */
eq("keepA", removeSplit(rig, "keepA"), ["gate", "amp", "eq", "reverb"]);
eq("keepB", removeSplit(rig, "keepB"), ["gate", "amp2", "reverb"]);
eq("series", removeSplit(rig, "series"), ["gate", "amp", "eq", "amp2", "reverb"]);
eq("series = flatten", removeSplit(rig, "series"), flattenStructure(rig));
for (const how of ["keepA", "keepB", "series"] as const) {
  valid(`removeSplit ${how}`, removeSplit(rig, how));
  ok(`removeSplit ${how} leaves no structure`, findStructure(removeSplit(rig, how)).splitAt < 0);
}
eq("keepA on empty fork", removeSplit(["amp", "split", "lane2", "mix"], "keepA"), ["amp"]);
eq("keepB with empty lane B", removeSplit(["split", "amp", "lane2", "mix"], "keepB"), []);
eq("malformed flattens", removeSplit(["gate", "split", "amp2", "cab"], "keepB"), ["gate", "amp2", "cab"]);
eq("no structure is identity", removeSplit(["amp", "cab"], "keepA"), ["amp", "cab"]);

/* blocksDroppedBy */
eq("drops for keepA", blocksDroppedBy(rig, "keepA"), ["amp2"]);
eq("drops for keepB", blocksDroppedBy(rig, "keepB"), ["amp", "eq"]);
eq("drops for series", blocksDroppedBy(rig, "series"), []);

/* swapLanes */
eq("swap", swapLanes(rig), ["gate", "split", "amp2", "lane2", "amp", "eq", "mix", "reverb"]);
eq("swap twice is identity", swapLanes(swapLanes(rig)), rig);
eq("swap empty lane", swapLanes(["split", "amp", "lane2", "mix"]), ["split", "lane2", "amp", "mix"]);
eq("swap without structure", swapLanes(["amp", "cab"]), ["amp", "cab"]);
valid("swap", swapLanes(rig));

/* dualAmpOrder */
const wrapped = dualAmpOrder(["gate", "amp", "cab"], 0);
eq("wrap amp in place", wrapped?.order ?? [], ["gate", "split", "amp", "lane2", "amp2", "mix", "cab"]);
ok("wrap reports position", wrapped?.insertAt === 1 && wrapped.added === 4);
valid("wrap", wrapped?.order ?? []);
const inserted = dualAmpOrder(["gate", "cab"], 1);
eq("insert rig at index", inserted?.order ?? [], ["gate", "split", "amp", "lane2", "amp2", "mix", "cab"]);
ok("insert reports position", inserted?.insertAt === 1 && inserted.added === 5);
ok("no rig when a split exists", dualAmpOrder(insertSplit(["amp"], 1), 0) === null);
ok("no rig when amp2 is placed", dualAmpOrder(["amp", "amp2"], 0) === null);
const full: BlockId[] = Array.from({ length: MAX_CHAIN_LENGTH - 4 }, () => "eq");
ok("wrap fits at cap-4", dualAmpOrder(["amp", ...full.slice(1)], 0) !== null);
ok("insert needs five free", dualAmpOrder(full, 0) === null);
ok("insert fits with five free", dualAmpOrder(full.slice(1), 0) !== null);

if (failures > 0) {
  console.error(`${failures} failed, ${passes} passed`);
  throw new Error("structure tests failed");
}
console.log(`structure tests: ${passes} passed`);
