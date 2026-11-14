#!/usr/bin/env node
// Decides whether HEAD releases, and as which version, from Conventional Commits since the
// last vX.Y.Z tag that change something the installer ships. Git tags are the only record
// of released versions.
//   node scripts/release/plan.mjs [--base-version 0.1.0] [--squash-title "<title>"] [--summary-label "<text>"]
// --squash-title plans HEAD, a pull request's merge commit, as if the pull request were
// squashed with that title; its $GITHUB_OUTPUT keys then start with squash_.
// Prints the plan as JSON and, under GitHub Actions, writes it to $GITHUB_OUTPUT.
import { execFileSync } from 'node:child_process';
import { appendFileSync } from 'node:fs';
import { parseArgs } from 'node:util';
import { pathToFileURL } from 'node:url';
import {
  bumpFor,
  bumpVersion,
  compareVersions,
  formatVersion,
  highestBump,
  parseCommit,
  parseVersion,
} from './conventional.mjs';

const git = (cwd, ...args) => execFileSync('git', args, { cwd, encoding: 'utf8' }).trim();

function tryGit(cwd, ...args) {
  try {
    return git(cwd, ...args);
  } catch {
    return null;
  }
}

function versionTags(cwd, ...filter) {
  return git(cwd, 'tag', '--list', 'v*', ...filter)
    .split('\n')
    .map((tag) => ({ tag, version: parseVersion(tag.slice(1)) }))
    .filter((t) => t.version)
    .sort((a, b) => compareVersions(b.version, a.version));
}

// Paths that never reach the installer. A commit counts toward a release only if it changes
// a path outside these, so the plan and the release notes agree on which commits count.
export const NOT_SHIPPED = [
  /^site\//,
  /^docs\//,
  /^\.github\//,
  /^\.audit\//,
  /^scripts\/release\//,
  /\.md$/i,
  /(^|\/)\.gitignore$/,
];

export const changesInstaller = (paths) => paths.some((path) => !NOT_SHIPPED.some((re) => re.test(path)));

const RECORD = '\x1e';
const FILES = '\x1f';
const splitPaths = (out) => out.split('\0').map((p) => p.trim()).filter(Boolean);

// A merge commit's paths are its diff against the first parent: what it brought to main.
function readCommits(cwd, range) {
  const out = execFileSync(
    'git',
    ['log', '-z', '--diff-merges=first-parent', '--name-only', `--format=${RECORD}%H%n%B${FILES}`, ...range],
    { cwd, encoding: 'utf8', maxBuffer: 256 * 1024 * 1024 },
  );
  return out
    .split(RECORD)
    .filter((entry) => entry.trim())
    .map((entry) => {
      const [head, files = ''] = entry.split(FILES);
      const newline = head.indexOf('\n');
      return { sha: head.slice(0, newline).trim(), message: head.slice(newline + 1), paths: splitPaths(files) };
    });
}

// The repository squashes with the commit messages as the body, so their footers still count.
function squashedPullRequest(cwd, title) {
  const parents = git(cwd, 'rev-list', '--parents', '-n', '1', 'HEAD').split(' ').slice(1);
  if (parents.length !== 2) throw new Error('--squash-title needs HEAD to be the merge commit of a pull request');
  const squashed = readCommits(cwd, ['HEAD^1..HEAD^2']);
  return {
    sha: git(cwd, 'rev-parse', 'HEAD'),
    message: [title, ...squashed.map((c) => c.message.trim())].join('\n\n'),
    paths: splitPaths(execFileSync('git', ['diff', '-z', '--name-only', 'HEAD^1', 'HEAD'], { cwd, encoding: 'utf8' })),
  };
}

function candidateCommits(cwd, previousTag, squashTitle) {
  if (squashTitle == null) return readCommits(cwd, previousTag ? [`${previousTag}..HEAD`] : ['HEAD']);
  const onBase = readCommits(cwd, previousTag ? [`${previousTag}..HEAD^1`] : ['HEAD^1']);
  return [squashedPullRequest(cwd, squashTitle), ...onBase];
}

export function planRelease({ cwd = process.cwd(), baseVersion = '0.1.0', squashTitle = null } = {}) {
  if (!parseVersion(baseVersion)) throw new Error(`base version must be X.Y.Z, got "${baseVersion}"`);
  if (git(cwd, 'rev-parse', '--is-shallow-repository') === 'true') {
    throw new Error('the checkout is shallow; fetch the full history and tags (actions/checkout fetch-depth: 0)');
  }

  const sha = git(cwd, 'rev-parse', 'HEAD');
  const headTags = versionTags(cwd, '--points-at', 'HEAD');
  const headTagNames = new Set(headTags.map((t) => t.tag));
  const previous = versionTags(cwd, '--merged', 'HEAD').find((t) => !headTagNames.has(t.tag)) ?? null;

  const parsed = candidateCommits(cwd, previous?.tag, squashTitle).flatMap(({ sha: commitSha, message, paths }) => {
    const commit = parseCommit(message);
    return commit ? [{ sha: commitSha, ...commit, paths }] : [];
  });
  for (const commit of parsed) {
    for (const value of commit.invalidReleaseAs) {
      console.error(`plan: warning: ignoring Release-As "${value}" in ${commit.sha.slice(0, 7)}; it must be X.Y.Z`);
    }
  }
  const commits = parsed
    .filter((c) => changesInstaller(c.paths))
    .map(({ paths, invalidReleaseAs, ...c }) => c);

  const base = {
    sha,
    previousTag: previous?.tag ?? null,
    commits,
  };

  if (headTags.length > 0) {
    const version = formatVersion(headTags[0].version);
    return {
      ...base,
      release: true,
      rerun: true,
      version,
      tag: `v${version}`,
      buildVersion: version,
      bump: null,
      reason: `HEAD is already tagged v${version}; publishing it again completes any missing assets`,
    };
  }

  const currentMajor = previous ? previous.version[0] : 0;
  const bump = highestBump(commits.map((c) => bumpFor(c, currentMajor)));
  // Release-As is an explicit request, so it counts whatever the commit touched.
  const releaseAs = parsed.find((c) => c.releaseAs)?.releaseAs ?? null;

  if (!bump && !releaseAs) {
    const buildVersion = previous ? formatVersion(previous.version) : baseVersion;
    return {
      ...base,
      release: false,
      rerun: false,
      version: null,
      tag: null,
      buildVersion,
      bump: null,
      reason: `no feat, fix, perf, breaking or Release-As commit that changes the installer since ${previous?.tag ?? 'the first commit'}`,
    };
  }

  let next;
  if (releaseAs) {
    next = parseVersion(releaseAs);
    if (previous && compareVersions(next, previous.version) <= 0) {
      throw new Error(`Release-As ${releaseAs} is not above the last release ${previous.tag}`);
    }
  } else {
    next = previous ? bumpVersion(previous.version, bump) : parseVersion(baseVersion);
  }

  const version = formatVersion(next);
  const tag = `v${version}`;
  const taggedCommit = tryGit(cwd, 'rev-parse', '-q', '--verify', `refs/tags/${tag}^{commit}`);
  if (taggedCommit) {
    throw new Error(`${tag} already exists on ${taggedCommit}, which is not HEAD (${sha}); refusing to release it again`);
  }

  return {
    ...base,
    release: true,
    rerun: false,
    version,
    tag,
    buildVersion: version,
    bump: releaseAs ? null : bump,
    reason: releaseAs
      ? `Release-As footer asks for ${version}`
      : previous
        ? `${bump} bump from ${previous.tag}`
        : `first release, at the base version ${version}`,
  };
}

function writeGithubOutput(plan, { keyPrefix, summaryLabel }) {
  if (!process.env.GITHUB_OUTPUT) return;
  const lines = {
    release: plan.release,
    rerun: plan.rerun,
    version: plan.version ?? '',
    tag: plan.tag ?? '',
    previous_tag: plan.previousTag ?? '',
    build_version: plan.buildVersion,
    sha: plan.sha,
  };
  appendFileSync(
    process.env.GITHUB_OUTPUT,
    Object.entries(lines).map(([k, v]) => `${keyPrefix}${k}=${v}\n`).join(''),
  );
  if (process.env.GITHUB_STEP_SUMMARY) {
    const headline = plan.release ? `Release **${plan.tag}**` : 'No release';
    const label = summaryLabel ? `**${summaryLabel}.** ` : '';
    appendFileSync(process.env.GITHUB_STEP_SUMMARY, `${label}${headline}: ${plan.reason}.\n\n`);
  }
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const { values } = parseArgs({
    options: {
      'base-version': { type: 'string', default: '0.1.0' },
      'squash-title': { type: 'string' },
      'summary-label': { type: 'string' },
    },
  });
  try {
    const squashTitle = values['squash-title'] ?? null;
    const plan = planRelease({ baseVersion: values['base-version'], squashTitle });
    writeGithubOutput(plan, { keyPrefix: squashTitle == null ? '' : 'squash_', summaryLabel: values['summary-label'] });
    console.log(JSON.stringify(plan, null, 2));
  } catch (error) {
    console.error(`plan: ${error.message}`);
    process.exit(1);
  }
}
