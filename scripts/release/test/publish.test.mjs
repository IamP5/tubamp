import { test } from 'node:test';
import assert from 'node:assert/strict';
import { chmodSync, copyFileSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fakeInstallers, runScript, scriptsDir, tempDir } from './helpers.mjs';

const SHA = 'a'.repeat(40);
const OTHER = 'b'.repeat(40);
const ALL = ['tubamp-0.2.0-macos-arm64.pkg', 'tubamp-0.2.0-macos-x86_64.pkg', 'SHA256SUMS', 'manifest.json'];

function github(initial = {}) {
  const dir = tempDir('tubamp-release-gh-');
  const bin = join(dir, 'bin');
  mkdirSync(bin);
  copyFileSync(join(scriptsDir, 'test', 'fake-gh.mjs'), join(bin, 'gh'));
  chmodSync(join(bin, 'gh'), 0o755);
  const statePath = join(dir, 'state.json');
  writeFileSync(statePath, JSON.stringify({ tags: {}, releases: [], calls: [], ...initial }));
  return {
    env: (extra = {}) => ({ PATH: `${bin}:${process.env.PATH}`, FAKE_GH_STATE: statePath, ...extra }),
    state: () => JSON.parse(readFileSync(statePath, 'utf8')),
    mutations: () =>
      JSON.parse(readFileSync(statePath, 'utf8')).calls.filter((c) => c[0] === 'release'),
    resetCalls() {
      const s = JSON.parse(readFileSync(statePath, 'utf8'));
      writeFileSync(statePath, JSON.stringify({ ...s, calls: [] }));
    },
  };
}

function releaseAssets() {
  const { dir } = fakeInstallers('0.2.0');
  const result = runScript('manifest.mjs', {
    args: ['--version', '0.2.0', '--dir', dir, '--notes-file', join(dir, 'notes.md')],
  });
  assert.equal(result.code, 0, result.stderr);
  return dir;
}

function publish(gh, dir, { extra = [], env = {} } = {}) {
  return runScript('publish.mjs', {
    args: ['--version', '0.2.0', '--sha', SHA, '--dir', dir, '--notes-file', join(dir, 'notes.md'), '--repo', 'o/r', ...extra],
    env: gh.env(env),
  });
}

const assetNames = (release) => release.assets.map((a) => a.name).sort();

test('a fresh publish uploads everything to a draft, manifest last, then publishes', () => {
  const gh = github();
  const dir = releaseAssets();
  const result = publish(gh, dir);
  assert.equal(result.code, 0, result.stderr);

  const [release] = gh.state().releases;
  assert.equal(release.draft, false);
  assert.deepEqual(assetNames(release), [...ALL].sort());
  assert.equal(gh.state().tags['v0.2.0'], SHA);

  const steps = gh.mutations().map((c) => c.slice(0, 2).join(' '));
  assert.deepEqual(steps, ['release create', 'release upload', 'release upload', 'release edit']);
  const uploads = gh.mutations().filter((c) => c[1] === 'upload');
  assert.ok(uploads[1].some((a) => a.endsWith('manifest.json')));
  assert.ok(!uploads[0].some((a) => a.endsWith('manifest.json')));
});

test('running it again after success changes nothing', () => {
  const gh = github();
  const dir = releaseAssets();
  publish(gh, dir);
  gh.resetCalls();
  const result = publish(gh, dir);
  assert.equal(result.code, 0, result.stderr);
  assert.deepEqual(gh.mutations(), []);
  assert.match(result.stdout, /already published/);
});

test('a crash while uploading the manifest leaves a draft that the rerun completes', () => {
  const gh = github();
  const dir = releaseAssets();
  const crashed = publish(gh, dir, { env: { FAKE_GH_CRASH_ON: 'manifest.json' } });
  assert.equal(crashed.code, 1);
  const [draft] = gh.state().releases;
  assert.equal(draft.draft, true, 'an incomplete release is never published');
  assert.equal(gh.state().latest, undefined);
  assert.equal(gh.state().tags['v0.2.0'], undefined);

  const rerun = publish(gh, dir);
  assert.equal(rerun.code, 0, rerun.stderr);
  assert.equal(gh.state().releases.length, 1, 'no duplicate release');
  assert.equal(gh.state().releases[0].draft, false);
  assert.deepEqual(assetNames(gh.state().releases[0]), [...ALL].sort());
  assert.equal(gh.state().latest, 'v0.2.0');
});

test('a crash after creating the draft is completed without a second draft', () => {
  const gh = github();
  const dir = releaseAssets();
  publish(gh, dir, { env: { FAKE_GH_CRASH_ON: 'tubamp-0.2.0-macos-arm64.pkg' } });
  assert.deepEqual(assetNames(gh.state().releases[0]), []);
  assert.equal(publish(gh, dir).code, 0);
  assert.equal(gh.state().releases.length, 1);
  assert.deepEqual(assetNames(gh.state().releases[0]), [...ALL].sort());
});

test('a published release with a missing asset gets the full set again', () => {
  const gh = github({
    tags: { 'v0.2.0': SHA },
    releases: [{ id: 1, tag_name: 'v0.2.0', draft: false, target: SHA, assets: [{ name: ALL[0], state: 'uploaded' }] }],
  });
  const dir = releaseAssets();
  assert.equal(publish(gh, dir).code, 0);
  assert.deepEqual(assetNames(gh.state().releases[0]), [...ALL].sort());
  assert.ok(!gh.mutations().some((c) => c[1] === 'create' || c[1] === 'edit'));
});

test('a tag on another commit stops the publish before any change', () => {
  const gh = github({ tags: { 'v0.2.0': OTHER } });
  const dir = releaseAssets();
  const result = publish(gh, dir);
  assert.equal(result.code, 1);
  assert.match(result.stderr, /already points at b{40}/);
  assert.deepEqual(gh.mutations(), []);
});

test('a missing installer stops the publish before any change', () => {
  const gh = github();
  const dir = releaseAssets();
  rmSync(join(dir, 'tubamp-0.2.0-macos-x86_64.pkg'));
  const result = publish(gh, dir);
  assert.equal(result.code, 1);
  assert.match(result.stderr, /missing asset/);
  assert.deepEqual(gh.state().calls, []);
});

test('a dry run reads the state and changes nothing', () => {
  const gh = github();
  const dir = releaseAssets();
  const result = publish(gh, dir, { extra: ['--dry-run'] });
  assert.equal(result.code, 0, result.stderr);
  assert.deepEqual(gh.mutations(), []);
  assert.match(result.stdout, /would run: gh release create v0\.2\.0 --draft/);
  assert.match(result.stdout, /would run: gh release edit v0\.2\.0 --draft=false --latest/);
});
