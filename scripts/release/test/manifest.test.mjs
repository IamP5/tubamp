import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { fakeInstallers, runScript } from './helpers.mjs';

const generate = (dir, version, extra = []) =>
  runScript('manifest.mjs', {
    args: ['--version', version, '--dir', dir, '--notes-file', join(dir, 'notes.md'), '--repo', 'IamP5/tubamp', ...extra],
  });

test('the manifest describes both installers exactly', () => {
  const { dir, files } = fakeInstallers('0.2.0');
  const result = generate(dir, '0.2.0', ['--published-at', '2026-11-14T16:58:00Z']);
  assert.equal(result.code, 0, result.stderr);
  const manifest = JSON.parse(readFileSync(join(dir, 'manifest.json'), 'utf8'));
  const download = (arch) => ({
    ...files[arch],
    url: `https://github.com/IamP5/tubamp/releases/download/v0.2.0/${files[arch].name}`,
  });
  assert.deepEqual(manifest, {
    schema: 1,
    repo: 'IamP5/tubamp',
    version: '0.2.0',
    tag: 'v0.2.0',
    publishedAt: '2026-11-14T16:58:00Z',
    url: 'https://github.com/IamP5/tubamp/releases/tag/v0.2.0',
    notes: '## Features\n\n- a plate reverb\n',
    downloads: { arm64: download('arm64'), x86_64: download('x86_64') },
    minimumMacOS: '11.0',
    signed: false,
  });
});

test('SHA256SUMS matches shasum -c format', () => {
  const { dir, files } = fakeInstallers('0.2.0');
  generate(dir, '0.2.0');
  assert.equal(
    readFileSync(join(dir, 'SHA256SUMS'), 'utf8'),
    `${files.arm64.sha256}  ${files.arm64.name}\n${files.x86_64.sha256}  ${files.x86_64.name}\n`,
  );
});

test('publishedAt defaults to now', () => {
  const { dir } = fakeInstallers('0.2.0');
  const before = Date.now() - 1000;
  generate(dir, '0.2.0');
  const { publishedAt } = JSON.parse(readFileSync(join(dir, 'manifest.json'), 'utf8'));
  assert.match(publishedAt, /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/);
  assert.ok(Date.parse(publishedAt) >= before);
});

test('a missing installer fails and writes nothing', () => {
  const { dir } = fakeInstallers('0.2.0');
  rmSync(join(dir, 'tubamp-0.2.0-macos-x86_64.pkg'));
  const result = generate(dir, '0.2.0');
  assert.equal(result.code, 1);
  assert.match(result.stderr, /missing installer .*x86_64\.pkg/);
  assert.throws(() => readFileSync(join(dir, 'manifest.json')));
  assert.throws(() => readFileSync(join(dir, 'SHA256SUMS')));
});

test('installers built for another version fail', () => {
  const { dir } = fakeInstallers('0.2.0');
  const result = generate(dir, '0.3.0');
  assert.equal(result.code, 1);
  assert.match(result.stderr, /missing installer/);
});

test('installers that disagree on the minimum macOS fail', () => {
  const { dir } = fakeInstallers('0.2.0');
  writeFileSync(
    join(dir, 'tubamp-0.2.0-macos-x86_64.build.json'),
    JSON.stringify({ version: '0.2.0', arch: 'x86_64', minimumMacOS: '10.13' }),
  );
  const result = generate(dir, '0.2.0');
  assert.equal(result.code, 1);
  assert.match(result.stderr, /minimum macOS/);
});
