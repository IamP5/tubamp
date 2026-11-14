#!/usr/bin/env node
// Decides whether HEAD releases, and as which version, from Conventional Commits since the
// last vX.Y.Z tag. Git tags are the only record of released versions.
//   node scripts/release/plan.mjs [--base-version 0.1.0]
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

function readCommits(cwd, range) {
  const out = execFileSync('git', ['log', '-z', '--format=%H%n%B', ...range], { cwd, encoding: 'utf8' });
  return out
    .split('\0')
    .filter((entry) => entry.trim())
    .map((entry) => {
      const newline = entry.indexOf('\n');
      return { sha: entry.slice(0, newline).trim(), message: entry.slice(newline + 1) };
    });
}

export function planRelease({ cwd = process.cwd(), baseVersion = '0.1.0' } = {}) {
  if (!parseVersion(baseVersion)) throw new Error(`base version must be X.Y.Z, got "${baseVersion}"`);
  if (git(cwd, 'rev-parse', '--is-shallow-repository') === 'true') {
    throw new Error('the checkout is shallow; fetch the full history and tags (actions/checkout fetch-depth: 0)');
  }

  const sha = git(cwd, 'rev-parse', 'HEAD');
  const headTags = versionTags(cwd, '--points-at', 'HEAD');
  const headTagNames = new Set(headTags.map((t) => t.tag));
  const previous = versionTags(cwd, '--merged', 'HEAD').find((t) => !headTagNames.has(t.tag)) ?? null;
  const range = previous ? [`${previous.tag}..HEAD`] : ['HEAD'];

  const commits = readCommits(cwd, range).flatMap(({ sha: commitSha, message }) => {
    const parsed = parseCommit(message);
    return parsed ? [{ sha: commitSha, ...parsed }] : [];
  });

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
  const releaseAs = commits.find((c) => c.releaseAs)?.releaseAs ?? null;

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
      reason: `no feat, fix, perf, breaking or Release-As commit since ${previous?.tag ?? 'the first commit'}`,
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

function writeGithubOutput(plan) {
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
    Object.entries(lines).map(([k, v]) => `${k}=${v}\n`).join(''),
  );
  if (process.env.GITHUB_STEP_SUMMARY) {
    const headline = plan.release ? `Release **${plan.tag}**` : 'No release';
    appendFileSync(process.env.GITHUB_STEP_SUMMARY, `${headline}: ${plan.reason}.\n`);
  }
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const { values } = parseArgs({ options: { 'base-version': { type: 'string', default: '0.1.0' } } });
  try {
    const plan = planRelease({ baseVersion: values['base-version'] });
    writeGithubOutput(plan);
    console.log(JSON.stringify(plan, null, 2));
  } catch (error) {
    console.error(`plan: ${error.message}`);
    process.exit(1);
  }
}
