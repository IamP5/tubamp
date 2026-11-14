import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { createHash, createPublicKey, generateKeyPairSync, verify } from 'node:crypto';
import { chmodSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { assembleEnvelope, keyIdOf, parseEnvelope, payloadFor, signedMessage } from '../envelope.mjs';
import { fakeInstallers, runScript, tempDir } from './helpers.mjs';

const openssl = process.env.OPENSSL || 'openssl';
const opensslBanner = spawnSync(openssl, ['version'], { encoding: 'utf8' }).stdout ?? '';
const deterministic = /^OpenSSL (3\.([2-9]|\d\d)|[4-9])\./.test(opensslBanner);
const skipSigning = deterministic ? false : `needs OpenSSL 3.2 or later, found "${opensslBanner.trim()}"`;

const ecKey = () => generateKeyPairSync('ec', { namedCurve: 'prime256v1' }).privateKey.export({ type: 'sec1', format: 'pem' });

function archiveDir({ minimumMacOS = '11.0' } = {}) {
  const { dir } = fakeInstallers('0.4.0', { minimumMacOS, updates: true });
  const zip = Buffer.from('a universal tubamp.component zip');
  writeFileSync(join(dir, 'tubamp-0.4.0-macos-universal.zip'), zip);
  return { dir, zip };
}

const envelope = (dir, keys, env = {}) =>
  runScript('envelope.mjs', { args: ['--version', '0.4.0', '--dir', dir], env: { TUBAMP_UPDATE_SIGNING_KEYS: keys, OPENSSL: openssl, ...env } });

test('the envelope has the layout docs/UPDATES.md gives, byte for byte', () => {
  const payload = payloadFor({ version: '0.4.0', minimumMacOS: '11.0', sha256: 'ab'.repeat(32), size: 17345678 });
  assert.equal(
    payload,
    `{"product":"com.t0audio.tubamp","version":"0.4.0","minimumMacOS":"11.0","archive":{"sha256":"${'ab'.repeat(32)}","size":17345678}}`,
  );
  const bytes = assembleEnvelope(payload, [{ keyId: '0011223344556677', der: Buffer.from([1, 2, 3]) }]);
  assert.equal(bytes.toString(), `tubamp-update-v1\nsig 0011223344556677 AQID\n.\n${payload}`);
  assert.deepEqual(parseEnvelope(bytes).fields.archive, { sha256: 'ab'.repeat(32), size: 17345678 });
  assert.equal(signedMessage(payload).toString(), `tubamp-update-v1\n${payload}`);
});

test('the key id is the one the client derives from the 65-byte point', () => {
  const pem = ecKey();
  const point = createPublicKey(pem).export({ type: 'spki', format: 'der' }).subarray(-65);
  assert.equal(point[0], 4);
  assert.equal(keyIdOf(pem), createHash('sha256').update(point).digest('hex').slice(0, 16));
});

test('a signed envelope verifies, names the archive, and is the same file on a rerun', { skip: skipSigning }, () => {
  const { dir, zip } = archiveDir();
  const pem = ecKey();
  const first = envelope(dir, pem);
  assert.equal(first.code, 0, first.stderr);
  const bytes = readFileSync(join(dir, 'update-v1.signed'));
  const { signatures, payload, fields } = parseEnvelope(bytes);
  assert.equal(signatures.length, 1);
  assert.equal(signatures[0].keyId, keyIdOf(pem));
  assert.ok(verify('sha256', signedMessage(payload), createPublicKey(pem), signatures[0].der), 'the signature verifies over magic + payload');
  assert.deepEqual(fields, {
    product: 'com.t0audio.tubamp',
    version: '0.4.0',
    minimumMacOS: '11.0',
    archive: { sha256: createHash('sha256').update(zip).digest('hex'), size: zip.length },
  });
  assert.doesNotMatch(first.stdout + first.stderr, /PRIVATE KEY/, 'the key never reaches the log');

  assert.equal(envelope(dir, pem).code, 0);
  assert.ok(readFileSync(join(dir, 'update-v1.signed')).equals(bytes), 'the deterministic nonce makes a rerun byte-identical');
});

test('two keys during a rotation give two signature lines', { skip: skipSigning }, () => {
  const { dir } = archiveDir();
  const [a, b] = [ecKey(), ecKey()];
  assert.equal(envelope(dir, `${a}\n${b}`).code, 0);
  const { signatures } = parseEnvelope(readFileSync(join(dir, 'update-v1.signed')));
  assert.deepEqual(signatures.map((s) => s.keyId), [keyIdOf(a), keyIdOf(b)]);
});

test('the signer refuses what it must not sign with', async (t) => {
  const rsa = generateKeyPairSync('rsa', { modulusLength: 2048 }).privateKey.export({ type: 'pkcs8', format: 'pem' });
  const p384 = generateKeyPairSync('ec', { namedCurve: 'secp384r1' }).privateKey.export({ type: 'sec1', format: 'pem' });
  const publicOnly = createPublicKey(ecKey()).export({ type: 'spki', format: 'pem' });
  const libressl = join(tempDir(), 'openssl');
  writeFileSync(libressl, '#!/bin/sh\necho "LibreSSL 3.3.6"\n');
  chmodSync(libressl, 0o755);
  const cases = [
    ['no key', '', {}, /no PEM private key/],
    ['a public key', publicOnly, {}, /no PEM private key/],
    ['an RSA key', rsa, {}, /EC P-256/],
    ['a P-384 key', p384, {}, /EC P-256/],
    ['an openssl without the deterministic nonce', ecKey(), { OPENSSL: libressl }, /need OpenSSL 3\.2 or later/],
  ];
  for (const [name, keys, env, message] of cases) {
    await t.test(name, () => {
      const { dir } = archiveDir();
      const result = envelope(dir, keys, env);
      assert.equal(result.code, 1);
      assert.match(result.stderr, message);
    });
  }
});

test('installers that disagree on the minimum macOS stop the signer', { skip: skipSigning }, () => {
  const { dir } = archiveDir();
  writeFileSync(join(dir, 'tubamp-0.4.0-macos-x86_64.build.json'), JSON.stringify({ version: '0.4.0', arch: 'x86_64', minimumMacOS: '12.0', updates: true }));
  const result = envelope(dir, ecKey());
  assert.equal(result.code, 1);
  assert.match(result.stderr, /disagree on the minimum macOS/);
});
