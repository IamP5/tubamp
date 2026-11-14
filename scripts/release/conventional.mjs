export const BUMP_BY_TYPE = {
  feat: 'minor',
  fix: 'patch',
  perf: 'patch',
  docs: null,
  ci: null,
  chore: null,
  refactor: null,
  test: null,
  build: null,
  style: null,
};

export const TYPES = Object.keys(BUMP_BY_TYPE);

const HEADER = /^([a-z]+)(?:\(([^()\r\n]+)\))?(!)?: (\S.*)$/;
const BREAKING_FOOTER = /^BREAKING[ -]CHANGE: *(.*)$/m;
const RELEASE_AS_FOOTER = /^Release-As: *v?(\S+) *$/im;
const VERSION = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;

export function parseVersion(text) {
  const m = VERSION.exec(text);
  return m ? m.slice(1, 4).map(Number) : null;
}

export function formatVersion([major, minor, patch]) {
  return `${major}.${minor}.${patch}`;
}

export function compareVersions(a, b) {
  return a[0] - b[0] || a[1] - b[1] || a[2] - b[2];
}

export function bumpVersion([major, minor, patch], bump) {
  if (bump === 'major') return [major + 1, 0, 0];
  if (bump === 'minor') return [major, minor + 1, 0];
  return [major, minor, patch + 1];
}

// Returns null for anything that is not a Conventional Commit with a known type,
// which includes this repo's older area-prefixed subjects such as "ui: ..." or "dsp: ...".
export function parseCommit(message) {
  const [header = '', ...rest] = message.trim().split('\n');
  const m = HEADER.exec(header.trim());
  if (!m || !TYPES.includes(m[1])) return null;
  const body = rest.join('\n');
  const breakingFooter = BREAKING_FOOTER.exec(body);
  const releaseAs = RELEASE_AS_FOOTER.exec(body);
  if (releaseAs && !parseVersion(releaseAs[1])) {
    throw new Error(`Release-As footer must be X.Y.Z, got "${releaseAs[1]}" in "${header}"`);
  }
  return {
    type: m[1],
    scope: m[2] ?? null,
    description: m[4].trim(),
    breaking: Boolean(m[3] || breakingFooter),
    breakingNote: breakingFooter?.[1].trim() || null,
    releaseAs: releaseAs ? releaseAs[1] : null,
  };
}

// While the major version is 0, a breaking change bumps the minor version.
export function bumpFor(commit, currentMajor) {
  if (commit.breaking) return currentMajor === 0 ? 'minor' : 'major';
  return BUMP_BY_TYPE[commit.type];
}

const RANK = { patch: 1, minor: 2, major: 3 };

export function highestBump(bumps) {
  return bumps.reduce((best, b) => (b && (!best || RANK[b] > RANK[best]) ? b : best), null);
}

export function lintTitle(title) {
  const commit = parseCommit(title ?? '');
  if (!commit || title.includes('\n')) {
    return {
      ok: false,
      error:
        `"${title}" is not a Conventional Commit title.\n` +
        `Use "<type>(<optional scope>)!: <description>" with a type from: ${TYPES.join(', ')}.\n` +
        'Examples: "feat(reverb): add a plate type", "fix: stop the gate chattering at 44.1 kHz".',
    };
  }
  return { ok: true, commit };
}
