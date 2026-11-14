import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { makeRepo, plan, runScript, tempDir } from './helpers.mjs';

test('history with only area prefixes and chores releases nothing', () => {
  const repo = makeRepo();
  repo.commit('ui: redesign the board');
  repo.commit('dsp: retune the gate');
  repo.commit('chore: bump deps');
  repo.commit('docs: explain presets');
  const p = plan(repo);
  assert.equal(p.release, false);
  assert.equal(p.version, null);
  assert.equal(p.buildVersion, '0.1.0');
});

test('the first releasable commit without any tag releases the base version', () => {
  const repo = makeRepo();
  repo.commit('ui: old style subject');
  repo.commit('fix: stop the gate chattering');
  const p = plan(repo);
  assert.equal(p.release, true);
  assert.equal(p.version, '0.1.0');
  assert.equal(p.tag, 'v0.1.0');
  assert.equal(p.previousTag, null);
});

test('the base version can be changed', () => {
  const repo = makeRepo();
  repo.commit('feat: first');
  assert.equal(plan(repo, ['--base-version', '1.0.0']).version, '1.0.0');
});

test('fix and perf bump the patch, feat the minor', () => {
  const repo = makeRepo();
  repo.commit('feat: base');
  repo.tag('v0.4.2');
  repo.commit('perf(nam): faster A2');
  assert.equal(plan(repo).version, '0.4.3');
  repo.commit('fix: a bug');
  assert.equal(plan(repo).version, '0.4.3');
  repo.commit('feat(reverb): plate');
  assert.equal(plan(repo).version, '0.5.0');
});

test('a breaking change bumps the minor while major is 0 and the major after 1.0', () => {
  const repo = makeRepo();
  repo.commit('feat: base');
  repo.tag('v0.4.2');
  repo.commit('refactor!: drop the classic engines');
  assert.equal(plan(repo).version, '0.5.0');

  const one = makeRepo();
  one.commit('feat: base');
  one.tag('v1.2.3');
  one.commit('fix: change the preset format\n\nBREAKING CHANGE: old presets no longer load');
  assert.equal(plan(one).version, '2.0.0');
});

test('Release-As forces the version, even on a chore', () => {
  const repo = makeRepo();
  repo.commit('feat: base');
  repo.tag('v0.9.1');
  repo.commit('chore: release 1.0.0\n\nRelease-As: 1.0.0');
  const p = plan(repo);
  assert.equal(p.release, true);
  assert.equal(p.version, '1.0.0');
});

test('Release-As at or below the last release fails', () => {
  const repo = makeRepo();
  repo.commit('feat: base');
  repo.tag('v0.9.1');
  repo.commit('chore: oops\n\nRelease-As: 0.9.1');
  const p = plan(repo);
  assert.equal(p.code, 1);
  assert.match(p.error, /not above/);
});

test('only commits since the last tag count, merge commits included', () => {
  const repo = makeRepo();
  repo.commit('feat: before');
  repo.tag('v0.2.0');
  repo.commit('docs: after');
  assert.equal(plan(repo).release, false);

  repo.git('checkout', '-q', '-b', 'topic');
  repo.commit('fix(ui): knob drag');
  repo.git('checkout', '-q', 'main');
  repo.commit('chore: unrelated');
  repo.git('merge', '-q', '--no-ff', '-m', 'Merge pull request #7 from topic', 'topic');
  const p = plan(repo);
  assert.equal(p.version, '0.2.1');
  assert.deepEqual(p.commits.map((c) => c.description).sort(), ['after', 'knob drag', 'unrelated']);
});

test('a squash commit is judged by its subject', () => {
  const repo = makeRepo();
  repo.commit('feat: base');
  repo.tag('v0.2.0');
  repo.commit('feat(delay): tape mode (#12)\n\n* wip\n* fix: typo');
  assert.equal(plan(repo).version, '0.3.0');
});

test('the highest tag reachable from HEAD is the last release', () => {
  const repo = makeRepo();
  repo.commit('feat: a');
  repo.tag('v0.2.0');
  repo.commit('feat: b');
  repo.tag('v0.10.0');
  repo.tag('not-a-version');
  repo.commit('fix: c');
  assert.equal(plan(repo).version, '0.10.1');
});

test('a tagged HEAD replans the same version as a rerun', () => {
  const repo = makeRepo();
  repo.commit('feat: a');
  repo.tag('v0.2.0');
  repo.commit('feat: b');
  repo.tag('v0.3.0');
  const p = plan(repo);
  assert.equal(p.release, true);
  assert.equal(p.rerun, true);
  assert.equal(p.version, '0.3.0');
  assert.equal(p.previousTag, 'v0.2.0');
  assert.deepEqual(p.commits.map((c) => c.description), ['b']);
});

test('the planned tag already existing on another commit fails loudly', () => {
  const repo = makeRepo();
  repo.commit('feat: a');
  repo.tag('v0.2.0');
  repo.git('checkout', '-q', '-b', 'elsewhere');
  repo.commit('fix: elsewhere');
  repo.tag('v0.2.1');
  repo.git('checkout', '-q', 'main');
  repo.commit('fix: here');
  const p = plan(repo);
  assert.equal(p.code, 1);
  assert.match(p.error, /v0\.2\.1 already exists/);
});

test('a shallow clone is refused', () => {
  const repo = makeRepo();
  repo.commit('feat: a');
  repo.commit('feat: b');
  const clone = join(tempDir(), 'clone');
  repo.git('clone', '-q', '--depth', '1', `file://${repo.dir}`, clone);
  const p = plan({ dir: clone });
  assert.equal(p.code, 1);
  assert.match(p.error, /shallow/);
});

test('the plan is written to GITHUB_OUTPUT', () => {
  const repo = makeRepo();
  const sha = repo.commit('feat: a');
  const out = join(tempDir(), 'output');
  const result = runScript('plan.mjs', { cwd: repo.dir, env: { GITHUB_OUTPUT: out } });
  assert.equal(result.code, 0);
  const lines = readFileSync(out, 'utf8');
  assert.match(lines, /^release=true$/m);
  assert.match(lines, /^version=0\.1\.0$/m);
  assert.match(lines, /^build_version=0\.1\.0$/m);
  assert.match(lines, new RegExp(`^sha=${sha}$`, 'm'));
});

test('release notes group commits and link each one', () => {
  const repo = makeRepo();
  repo.commit('feat: base');
  repo.tag('v0.2.0');
  const featSha = repo.commit('feat(reverb): add a plate type');
  const fixSha = repo.commit('fix: stop the gate chattering');
  repo.commit('perf(nam): skip the resampler at the native rate');
  repo.commit('feat!: new preset format\n\nBREAKING CHANGE: presets from 0.2 must be re-saved');
  repo.commit('docs: not in the notes');
  repo.commit('ui: not conventional');

  const result = runScript('notes.mjs', { cwd: repo.dir, args: ['--repo', 'o/r'] });
  assert.equal(result.code, 0);
  const notes = result.stdout;
  const order = ['## Breaking changes', '## Features', '## Fixes', '## Performance', '## Install'].map((h) =>
    notes.indexOf(h),
  );
  assert.ok(order.every((i, k) => i >= 0 && (k === 0 || i > order[k - 1])), notes);
  assert.ok(notes.includes(`- **reverb:** add a plate type ([${featSha.slice(0, 7)}](https://github.com/o/r/commit/${featSha}))`));
  assert.ok(notes.includes(`- stop the gate chattering ([${fixSha.slice(0, 7)}](https://github.com/o/r/commit/${fixSha}))`));
  assert.ok(notes.includes('presets from 0.2 must be re-saved'));
  assert.ok(notes.includes('tubamp-0.3.0-macos-arm64.pkg'));
  assert.ok(!notes.includes('not in the notes'));
  assert.ok(!notes.includes('not conventional'));
});

test('release notes say so when nothing is released', () => {
  const repo = makeRepo();
  repo.commit('docs: only docs');
  const result = runScript('notes.mjs', { cwd: repo.dir });
  assert.equal(result.code, 0);
  assert.match(result.stdout, /^Nothing to release/);
});
