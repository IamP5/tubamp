import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
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

test('a releasable commit that changes nothing in the installer releases nothing', () => {
  const repo = makeRepo();
  repo.commit('feat: base');
  repo.tag('v0.1.0');
  repo.commit('fix(site): typo', ['site/src/app/page.tsx']);
  repo.commit('feat: explain presets', ['docs/PRESETS.md']);
  repo.commit('fix: workflow', ['.github/workflows/release.yml']);
  repo.commit('fix: audit', ['.audit/landing-release.tsv']);
  repo.commit('fix: planner', ['scripts/release/plan.mjs']);
  repo.commit('perf: readmes', ['README.md', 'tools/fxbench/README.md']);
  repo.commit('feat: empty', []);
  const p = plan(repo);
  assert.equal(p.release, false, JSON.stringify(p));
  assert.deepEqual(p.commits, []);
});

test('what a commit touched decides, not its scope', () => {
  const repo = makeRepo();
  repo.commit('feat: base');
  repo.tag('v0.1.0');
  repo.commit('fix(site): deployment target', ['CMakeLists.txt']);
  assert.equal(plan(repo).version, '0.1.1');

  const other = makeRepo();
  other.commit('feat: base');
  other.tag('v0.1.0');
  other.commit('fix(release): build script', ['scripts/release/build-installer.sh', 'Source/PluginProcessor.cpp']);
  assert.equal(plan(other).version, '0.1.1');
});

test('a squash commit that touches the site and the plugin releases', () => {
  const repo = makeRepo();
  repo.commit('feat: base');
  repo.tag('v0.1.0');
  repo.commit('feat: download button and smoke flag (#9)', ['site/src/app/page.tsx', 'Source/smoke.cpp']);
  assert.equal(plan(repo).version, '0.2.0');
});

test('release notes list exactly the commits that counted', () => {
  const repo = makeRepo();
  repo.commit('feat: base');
  repo.tag('v0.1.0');
  repo.commit('fix(site): typo on the page', ['site/src/app/page.tsx']);
  repo.commit('fix(dsp): gate chatter', ['Source/Gate.cpp']);
  const result = runScript('notes.mjs', { cwd: repo.dir, args: ['--repo', 'o/r'] });
  assert.equal(result.code, 0, result.stderr);
  assert.match(result.stdout, /gate chatter/);
  assert.ok(!result.stdout.includes('typo on the page'), result.stdout);
});

test('an invalid Release-As footer warns and is ignored, and a later valid one wins', () => {
  const repo = makeRepo();
  repo.commit('feat: base');
  repo.tag('v0.9.1');
  repo.commit('chore: release one\n\nRelease-As: 1.0');
  let result = runScript('plan.mjs', { cwd: repo.dir });
  assert.equal(result.code, 0, result.stderr);
  assert.equal(JSON.parse(result.stdout).release, false);
  assert.match(result.stderr, /Release-As.*"1\.0"/);

  repo.commit('chore: release one, properly\n\nRelease-As: 1.0.0');
  result = runScript('plan.mjs', { cwd: repo.dir });
  assert.equal(result.code, 0, result.stderr);
  assert.equal(JSON.parse(result.stdout).version, '1.0.0');
});

function pullRequest(topicCommits) {
  const repo = makeRepo();
  repo.commit('feat: base');
  repo.tag('v0.1.0');
  repo.git('checkout', '-q', '-b', 'topic');
  for (const [message, files] of topicCommits) repo.commit(message, files);
  repo.git('checkout', '-q', 'main');
  repo.commit('chore: on main meanwhile', ['docs/x.md']);
  repo.git('merge', '-q', '--no-ff', '-m', 'Merge pull request #7 from topic', 'topic');
  return repo;
}

test('planning as a squash judges the range by the title', () => {
  const repo = pullRequest([['feat(reverb): plate', ['Source/Reverb.cpp']], ['fix: typo', ['Source/Reverb.h']]]);
  assert.equal(plan(repo).version, '0.2.0');
  const squashed = plan(repo, ['--squash-title', 'chore: tidy the reverb']);
  assert.equal(squashed.release, false, JSON.stringify(squashed));
  assert.equal(plan(repo, ['--squash-title', 'fix: tidy the reverb']).version, '0.1.1');
});

test('a squash touches the union of the range paths', () => {
  const site = pullRequest([['feat(site): hero', ['site/a.tsx']], ['feat(site): nav', ['site/b.tsx']]]);
  assert.equal(plan(site, ['--squash-title', 'feat: new landing page']).release, false);

  const both = pullRequest([['chore(site): hero', ['site/a.tsx']], ['chore: smoke flag', ['Source/smoke.cpp']]]);
  assert.equal(plan(both).release, false);
  assert.equal(plan(both, ['--squash-title', 'feat: hero and smoke flag']).version, '0.2.0');
});

test('a squash keeps the footers of the squashed commits', () => {
  const repo = pullRequest([['refactor: presets\n\nBREAKING CHANGE: old presets no longer load', ['Source/Presets.cpp']]]);
  assert.equal(plan(repo, ['--squash-title', 'chore: presets']).version, '0.2.0');
});

test('the squash plan is written to GITHUB_OUTPUT under squash_ keys', () => {
  const repo = pullRequest([['feat: plate', ['Source/Reverb.cpp']]]);
  const out = join(tempDir(), 'output');
  const result = runScript('plan.mjs', {
    cwd: repo.dir,
    args: ['--squash-title', 'feat: plate'],
    env: { GITHUB_OUTPUT: out },
  });
  assert.equal(result.code, 0, result.stderr);
  const lines = readFileSync(out, 'utf8');
  assert.match(lines, /^squash_release=true$/m);
  assert.match(lines, /^squash_build_version=0\.2\.0$/m);
  assert.doesNotMatch(lines, /^release=/m);
});

const KEY_FILES = ['src/update/keys/release.pub.pem', 'src/update/keys/recovery.pub.pem'];

test('with no public key committed the updater stays dormant, whatever the secret', () => {
  const repo = makeRepo();
  repo.commit('feat: plate', ['Source/Reverb.cpp', 'src/update/keys/README.md']);
  for (const key of ['present', 'absent', 'throwaway']) {
    const p = plan(repo, ['--signing-key', key]);
    assert.equal(p.release, true, key);
    assert.equal(p.updates, 'off', key);
    assert.match(p.updatesReason, /no public key is committed/);
  }
});

test('exactly one committed key stops the plan, because the recovery key is missing', () => {
  const repo = makeRepo();
  repo.commit('feat: plate', ['Source/Reverb.cpp', KEY_FILES[0]]);
  for (const key of ['present', 'absent', 'throwaway']) {
    const p = plan(repo, ['--signing-key', key]);
    assert.equal(p.code, 1, key);
    assert.match(p.error, /holds 1 public key .* recovery key/, key);
  }
});

test('an armed repository without the signing secret stops the plan instead of releasing with updates off', () => {
  const repo = makeRepo();
  repo.commit('feat: plate', ['Source/Reverb.cpp', ...KEY_FILES]);
  const absent = plan(repo, ['--signing-key', 'absent']);
  assert.equal(absent.code, 1);
  assert.match(absent.error, /2 public keys are committed .* no TUBAMP_UPDATE_SIGNING_KEYS secret/);
  assert.equal(plan(repo).code, 1, 'no flag means no secret');

  repo.commit('docs: explain presets', ['docs/presets.md']);
  assert.equal(plan(repo, ['--signing-key', 'absent']).code, 1, 'also when no release is due');
});

test('two committed keys arm the release with the signing secret or a dry run', () => {
  const repo = makeRepo();
  repo.commit('feat: plate', ['Source/Reverb.cpp', ...KEY_FILES]);
  assert.equal(plan(repo, ['--signing-key', 'present']).updates, 'on');
  assert.equal(plan(repo, ['--signing-key', 'throwaway']).updates, 'on');
  assert.match(plan(repo, ['--signing-key', 'maybe']).error, /--signing-key must be one of/);
});

test('keys that are only in the working tree do not arm a release', () => {
  const repo = makeRepo();
  repo.commit('feat: plate', ['Source/Reverb.cpp']);
  mkdirSync(join(repo.dir, 'src/update/keys'), { recursive: true });
  for (const file of KEY_FILES) writeFileSync(join(repo.dir, file), 'untracked');
  assert.equal(plan(repo, ['--signing-key', 'present']).updates, 'off');
});

test('the updates decision reaches GITHUB_OUTPUT and the job summary', () => {
  const repo = makeRepo();
  repo.commit('feat: plate', ['Source/Reverb.cpp']);
  const dir = tempDir();
  const result = runScript('plan.mjs', {
    cwd: repo.dir,
    args: ['--signing-key', 'present'],
    env: { GITHUB_OUTPUT: join(dir, 'output'), GITHUB_STEP_SUMMARY: join(dir, 'summary') },
  });
  assert.equal(result.code, 0, result.stderr);
  assert.match(readFileSync(join(dir, 'output'), 'utf8'), /^updates=off$/m);
  assert.match(readFileSync(join(dir, 'summary'), 'utf8'), /Updates \*\*off\*\*: no public key is committed/);
});
