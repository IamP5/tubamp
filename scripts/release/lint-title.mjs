#!/usr/bin/env node
// Checks a pull request title against the commit convention. The title arrives in the
// PR_TITLE environment variable so it never passes through a shell command line.
import { BUMP_BY_TYPE, lintTitle } from './conventional.mjs';

const title = process.env.PR_TITLE ?? '';
const result = lintTitle(title);

if (!result.ok) {
  console.error(result.error);
  process.exit(1);
}

const { commit } = result;
const effect = commit.breaking
  ? 'a breaking change (minor bump while the version is 0.x, major after 1.0)'
  : BUMP_BY_TYPE[commit.type]
    ? `a ${BUMP_BY_TYPE[commit.type]} release`
    : 'no release';
console.log(`ok: squash-merging with this title makes ${effect}.`);
