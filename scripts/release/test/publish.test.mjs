import { test } from 'node:test';
import assert from 'node:assert/strict';
import { chmodSync, copyFileSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join } from 'node:path';
import { assembleEnvelope, payloadFor } from '../envelope.mjs';
import { fakeInstallers, runScript, scriptsDir, tempDir } from './helpers.mjs';

const SHA = 'a'.repeat(40);
const OTHER = 'b'.repeat(40);
const ALL = ['tubamp-0.2.0-macos-arm64.pkg', 'tubamp-0.2.0-macos-x86_64.pkg', 'SHA256SUMS', 'manifest.json'];
const ZIP = 'tubamp-0.2.0-macos-universal.zip';
const ENVELOPE = 'update-v1.signed';
const ARMED = [...ALL, ZIP, ENVELOPE];

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
      JSON.parse(readFileSync(statePath, 'utf8')).calls.filter((c) => c[0] === 'release' || c.includes('DELETE')),
    resetCalls() {
      const s = JSON.parse(readFileSync(statePath, 'utf8'));
      writeFileSync(statePath, JSON.stringify({ ...s, calls: [] }));
    },
  };
}

function writeArchiveAndEnvelope(dir, { archiveBytes = Buffer.from('universal zip'), envelopeFor = archiveBytes, version = '0.2.0' } = {}) {
  writeFileSync(join(dir, ZIP), archiveBytes);
  const payload = payloadFor({
    version,
    minimumMacOS: '11.0',
    sha256: createHash('sha256').update(envelopeFor).digest('hex'),
    size: envelopeFor.length,
  });
  writeFileSync(join(dir, ENVELOPE), assembleEnvelope(payload, [{ keyId: '0123456789abcdef', der: Buffer.from('not a real signature') }]));
}

function releaseAssets({ updates = false } = {}) {
  const { dir } = fakeInstallers('0.2.0', { updates });
  const result = runScript('manifest.mjs', {
    args: ['--version', '0.2.0', '--dir', dir, '--notes-file', join(dir, 'notes.md')],
  });
  assert.equal(result.code, 0, result.stderr);
  if (updates) writeArchiveAndEnvelope(dir);
  return dir;
}

function publish(gh, dir, { extra = [], env = {}, updates = 'off' } = {}) {
  return runScript('publish.mjs', {
    args: ['--version', '0.2.0', '--sha', SHA, '--dir', dir, '--notes-file', join(dir, 'notes.md'), '--repo', 'o/r', '--updates', updates, ...extra],
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

test('a stale draft for another commit is replaced, not published', () => {
  const gh = github({
    releases: [{ id: 1, tag_name: 'v0.2.0', draft: true, target: OTHER, assets: ALL.map((name) => ({ name, state: 'uploaded' })) }],
  });
  const dir = releaseAssets();
  const result = publish(gh, dir);
  assert.equal(result.code, 0, result.stderr);
  const [release, ...rest] = gh.state().releases;
  assert.deepEqual(rest, []);
  assert.equal(release.draft, false);
  assert.equal(release.target, SHA);
  assert.equal(gh.state().tags['v0.2.0'], SHA);
  const uploaded = gh.mutations().filter((c) => c[1] === 'upload').flat().map((a) => a.split('/').pop());
  assert.deepEqual(ALL.filter((name) => uploaded.includes(name)), ALL, 'every asset comes from this run');
});

test('a published release on another commit still stops the run', () => {
  const gh = github({
    tags: { 'v0.2.0': OTHER },
    releases: [{ id: 1, tag_name: 'v0.2.0', draft: false, target: OTHER, assets: ALL.map((name) => ({ name, state: 'uploaded' })) }],
  });
  const result = publish(gh, releaseAssets());
  assert.equal(result.code, 1);
  assert.deepEqual(gh.mutations(), []);
  assert.equal(gh.state().releases.length, 1);
});

test('an armed release uploads the archive before the envelope, the envelope before the manifest', () => {
  const gh = github();
  const dir = releaseAssets({ updates: true });
  const result = publish(gh, dir, { updates: 'on' });
  assert.equal(result.code, 0, result.stderr);
  const [release] = gh.state().releases;
  assert.equal(release.draft, false);
  assert.deepEqual(assetNames(release), [...ARMED].sort());

  const uploads = gh.mutations().filter((c) => c[1] === 'upload').map((c) => c.filter((a) => a.startsWith(dir)).map((a) => a.split('/').pop()));
  assert.deepEqual(uploads, [[ALL[0], ALL[1], ZIP, 'SHA256SUMS'], [ENVELOPE], ['manifest.json']]);
  assert.deepEqual(gh.mutations().at(-1).slice(0, 2), ['release', 'edit'], 'published only after the last upload');
});

test('an armed release that crashes on the envelope stays a draft, and the rerun completes it', () => {
  const gh = github();
  const dir = releaseAssets({ updates: true });
  assert.equal(publish(gh, dir, { updates: 'on', env: { FAKE_GH_CRASH_ON: ENVELOPE } }).code, 1);
  assert.equal(gh.state().releases[0].draft, true);
  assert.equal(gh.state().latest, undefined, 'nothing is latest before every asset is up');
  assert.ok(!assetNames(gh.state().releases[0]).includes('manifest.json'));

  assert.equal(publish(gh, dir, { updates: 'on' }).code, 0);
  assert.equal(gh.state().releases.length, 1);
  assert.deepEqual(assetNames(gh.state().releases[0]), [...ARMED].sort());
  assert.equal(gh.state().latest, 'v0.2.0');

  gh.resetCalls();
  const again = publish(gh, dir, { updates: 'on' });
  assert.equal(again.code, 0, again.stderr);
  assert.deepEqual(gh.mutations(), [], 'a third run changes nothing');
});

test('half-armed inputs stop the publish before any call to GitHub', async (t) => {
  const cases = [
    ['updates on without the update assets', () => releaseAssets({ updates: true }), (dir) => rmSync(join(dir, ENVELOPE)), 'on', /missing asset .*update-v1\.signed/],
    ['updates on with installers built without the updater', () => releaseAssets(), (dir) => writeArchiveAndEnvelope(dir), 'on', /built with updates off but this release has --updates on/],
    ['updates off with installers built with the updater', () => releaseAssets({ updates: true }), (dir) => { rmSync(join(dir, ZIP)); rmSync(join(dir, ENVELOPE)); }, 'off', /built with updates on but this release has --updates off/],
    ['updates off with an envelope in the directory', () => releaseAssets(), (dir) => writeArchiveAndEnvelope(dir), 'off', /update-v1\.signed in .* --updates off/],
    ['an envelope that names another archive', () => releaseAssets({ updates: true }), (dir) => writeArchiveAndEnvelope(dir, { envelopeFor: Buffer.from('another zip') }), 'on', /names an archive of/],
    ['an envelope for another version', () => releaseAssets({ updates: true }), (dir) => writeArchiveAndEnvelope(dir, { version: '0.1.9' }), 'on', /is for com\.t0audio\.tubamp 0\.1\.9/],
    ['no --updates at all', () => releaseAssets(), () => {}, '', /--updates must be on or off/],
  ];
  for (const [name, make, spoil, updates, message] of cases) {
    await t.test(name, () => {
      const gh = github();
      const dir = make();
      spoil(dir);
      const result = publish(gh, dir, { updates });
      assert.equal(result.code, 1);
      assert.match(result.stderr, message);
      assert.deepEqual(gh.state().calls, []);
    });
  }
});

test('a published release that carries update assets is never republished with updates off', () => {
  const gh = github({
    tags: { 'v0.2.0': SHA },
    releases: [{ id: 1, tag_name: 'v0.2.0', draft: false, target: SHA, assets: ARMED.map((name) => ({ name, state: 'uploaded' })) }],
  });
  const result = publish(gh, releaseAssets());
  assert.equal(result.code, 1);
  assert.match(result.stderr, /is published with .*update-v1\.signed.*half-armed/);
  assert.deepEqual(gh.mutations(), []);
});

test('a draft an armed run left is replaced when the rerun has updates off', () => {
  const gh = github({
    releases: [{ id: 1, tag_name: 'v0.2.0', draft: true, target: SHA, assets: [ZIP, ENVELOPE, ALL[0]].map((name) => ({ name, state: 'uploaded' })) }],
  });
  const result = publish(gh, releaseAssets());
  assert.equal(result.code, 0, result.stderr);
  const [release, ...rest] = gh.state().releases;
  assert.deepEqual(rest, []);
  assert.equal(release.draft, false);
  assert.deepEqual(assetNames(release), [...ALL].sort(), 'exactly the four assets of an off release');
});

test('the dry run lists exactly four assets with updates off and six with updates on', () => {
  const off = publish(github(), releaseAssets(), { extra: ['--dry-run'] });
  assert.equal(off.code, 0, off.stderr);
  assert.match(off.stdout, /would publish v0\.2\.0 at a{40} with updates off: tubamp-0\.2\.0-macos-arm64\.pkg, tubamp-0\.2\.0-macos-x86_64\.pkg, SHA256SUMS, manifest\.json$/m);
  const on = publish(github(), releaseAssets({ updates: true }), { updates: 'on', extra: ['--dry-run'] });
  assert.equal(on.code, 0, on.stderr);
  assert.match(on.stdout, /with updates on: .*universal\.zip, SHA256SUMS, update-v1\.signed, manifest\.json$/m);
});
