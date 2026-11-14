// Fails on the visual patterns the design system bans (docs/REACT-UI.md
// §Design system).   node tools/design-lint.mjs <file or dir> [...]
// A line that needs an exception carries `ds-allow: <reason>` in a comment.
import { readdirSync, readFileSync, statSync } from "node:fs";
import { extname, join, relative } from "node:path";

const FONT_SIZES = new Set(["11", "12", "13", "15", "20", "28"]);
const RADII = new Set(["0", "2", "4", "6", "8", "12", "999"]);

const CSS_RULES = [
  [/#[0-9a-fA-F]{3,8}\b|\brgba?\(|\bhsla?\(/, "colour literal: use a --ds-* token"],
  [/gradient\(/, "gradient"],
  [/text-transform:\s*uppercase/, "uppercase text"],
  [/letter-spacing:\s*(?!0\b|normal\b)/, "letter-spacing"],
  [/backdrop-filter|filter:\s*[^;]*(blur|drop-shadow)/, "blur or drop-shadow filter"],
  [/text-shadow:/, "text-shadow"],
  [/box-shadow:(?![^;]*(var\(--ds-|none|inherit))/, "box-shadow outside the --ds-shadow/ring/focus tokens"],
  [/animation[^;]*\binfinite\b/, "looping animation"],
  [/font-family:(?![^;]*(var\(--ds-|inherit))/, "font-family literal: use --ds-font or --ds-mono"],
  [/font-style:\s*italic/, "italic"],
];

const TSX_RULES = [
  [/["'`]#[0-9a-fA-F]{3,8}["'`]|\brgba?\(\s*\d|\bhsla?\(\s*\d/, "colour literal: use a --ds-* token (var() works in SVG attributes set through style or a CSS class)"],
  [/\bBLOCK_ACCENT\b/, "per-block colour (BLOCK_ACCENT)"],
  [/\bartHues\b/, "generated tone-art gradient (artHues)"],
  [/[←-⇿⌀-⏿■-➿⬀-⯿\u{1f000}-\u{1faff}]/u, "unicode glyph used as an icon: draw an SVG icon"],
  [/textTransform:\s*["']uppercase/, "uppercase text"],
  [/letterSpacing:/, "letter-spacing"],
  [/repeat:\s*Infinity/, "looping animation"],
];

function walk(path, out) {
  const stat = statSync(path);
  if (stat.isDirectory()) {
    for (const name of readdirSync(path)) walk(join(path, name), out);
  } else if ([".css", ".ts", ".tsx"].includes(extname(path))) {
    out.push(path);
  }
  return out;
}

// Where the tokens and the font faces are defined: literals are the point.
const DEFINITIONS = /theme[\\/](tokens|fonts)\.css$/;

function check(file) {
  if (DEFINITIONS.test(file)) return [];
  const css = extname(file) === ".css";
  const rules = css ? CSS_RULES : TSX_RULES;
  const found = [];
  readFileSync(file, "utf8")
    .split("\n")
    .forEach((line, index) => {
      if (line.includes("ds-allow")) return;
      // Comments are prose, not UI: skip `//` tails and JSDoc / block lines.
      const code = css ? line : /^\s*(\*|\/\*)/.test(line) ? "" : line.replace(/\/\/.*$/, "");
      for (const [pattern, message] of rules) {
        if (pattern.test(code)) found.push([index + 1, message, line.trim()]);
      }
      if (!css) return;
      const size = /font-size:\s*([\d.]+)px/.exec(code);
      if (size && !FONT_SIZES.has(size[1])) found.push([index + 1, `font-size ${size[1]}px is off the scale (11 12 13 15 20 28)`, line.trim()]);
      const radius = /border-radius:\s*([^;]+)/.exec(code);
      if (radius && !radius[1].includes("var(") && !radius[1].includes("%")) {
        const off = radius[1].match(/[\d.]+(?=px)/g)?.filter((n) => !RADII.has(n));
        if (off?.length) found.push([index + 1, `border-radius ${off.join(" ")}px is off the scale (use --ds-r-*)`, line.trim()]);
      }
    });
  return found;
}

const targets = process.argv.slice(2);
if (!targets.length) {
  console.error("usage: node design-lint.mjs <file or dir> [...]");
  process.exit(2);
}

let total = 0;
for (const file of targets.flatMap((target) => walk(target, []))) {
  const found = check(file);
  total += found.length;
  for (const [line, message, text] of found) {
    console.log(`${relative(process.cwd(), file)}:${line}  ${message}\n    ${text.slice(0, 140)}`);
  }
}
console.log(total ? `\n${total} design violation(s)` : "design-lint: clean");
process.exit(total ? 1 : 0);
