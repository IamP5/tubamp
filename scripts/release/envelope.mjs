#!/usr/bin/env node
// Writes update-v1.signed, the signed file the in-plugin updater reads (docs/UPDATES.md),
// for the universal update archive in --dir.
//   TUBAMP_UPDATE_SIGNING_KEYS="<PEM>" node scripts/release/envelope.mjs --version X.Y.Z --dir dist
// The variable holds one PEM private key, or up to four during a key rotation. The key is
// read from the environment, never from the command line, and lives on disk only in a
// 0600 file that is deleted before the script exits. OPENSSL names the openssl binary
// (OpenSSL 3.2 or later, for the deterministic nonce); the default is `openssl`.
import { execFileSync } from 'node:child_process';
import { createHash, createPrivateKey, createPublicKey, verify } from 'node:crypto';
import { chmodSync, mkdtempSync, readFileSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { parseArgs } from 'node:util';
import { pathToFileURL } from 'node:url';
import { ARCHS, installerName } from './manifest.mjs';
import { parseVersion } from './conventional.mjs';

export const MAGIC = 'tubamp-update-v1';
export const ENVELOPE_NAME = 'update-v1.signed';
export const PRODUCT = 'com.t0audio.tubamp';
export const archiveName = (version) => `tubamp-${version}-macos-universal.zip`;

// The client's limits (src/update/detail/Rules.h).
const MAX_ENVELOPE = 16 * 1024;
const MAX_PAYLOAD = 8 * 1024;
const MAX_SIGNATURES = 4;

/** The payload's exact bytes. Field order is fixed so a rerun writes the same file. */
export function payloadFor({ version, minimumMacOS, sha256, size }) {
  return JSON.stringify({ product: PRODUCT, version, minimumMacOS, archive: { sha256, size } });
}

/** What each signature covers: the magic line, then the payload. */
export const signedMessage = (payload) => Buffer.from(`${MAGIC}\n${payload}`);

/** The first 8 bytes of SHA-256 over the 65-byte public point, in hex. */
export function keyIdOf(key) {
  const spki = createPublicKey(key).export({ type: 'spki', format: 'der' });
  return createHash('sha256').update(spki.subarray(spki.length - 65)).digest('hex').slice(0, 16);
}

export function assembleEnvelope(payload, signatures) {
  const lines = [MAGIC, ...signatures.map(({ keyId, der }) => `sig ${keyId} ${der.toString('base64')}`), '.'];
  return Buffer.from(`${lines.join('\n')}\n${payload}`);
}

/** Splits an envelope into its signature lines and payload, without checking a signature. */
export function parseEnvelope(bytes) {
  const text = Buffer.from(bytes).toString('utf8');
  if (bytes.length > MAX_ENVELOPE) throw new Error(`${ENVELOPE_NAME} is ${bytes.length} bytes, above ${MAX_ENVELOPE}`);
  const end = text.indexOf('\n.\n');
  if (end < 0) throw new Error(`${ENVELOPE_NAME} has no "." line`);
  const [magic, ...sigLines] = text.slice(0, end).split('\n');
  if (magic !== MAGIC) throw new Error(`${ENVELOPE_NAME} does not start with ${MAGIC}`);
  const signatures = sigLines.map((line) => {
    const match = /^sig ([0-9a-f]{16}) ([A-Za-z0-9+/=]+)$/.exec(line);
    if (!match) throw new Error(`${ENVELOPE_NAME} has a malformed line: ${line.slice(0, 40)}`);
    return { keyId: match[1], der: Buffer.from(match[2], 'base64') };
  });
  if (signatures.length < 1 || signatures.length > MAX_SIGNATURES) {
    throw new Error(`${ENVELOPE_NAME} has ${signatures.length} signature lines, expected 1 to ${MAX_SIGNATURES}`);
  }
  const payload = text.slice(end + 3);
  if (Buffer.byteLength(payload) > MAX_PAYLOAD) throw new Error(`${ENVELOPE_NAME} payload is above ${MAX_PAYLOAD} bytes`);
  return { signatures, payload, fields: JSON.parse(payload) };
}

export function splitPrivateKeys(text) {
  const blocks = (text ?? '').match(/-----BEGIN [A-Z ]*PRIVATE KEY-----[\s\S]*?-----END [A-Z ]*PRIVATE KEY-----/g) ?? [];
  if (blocks.length === 0) throw new Error('no PEM private key in TUBAMP_UPDATE_SIGNING_KEYS');
  if (blocks.length > MAX_SIGNATURES) throw new Error(`${blocks.length} keys, at most ${MAX_SIGNATURES}`);
  return blocks.map((pem) => {
    const key = createPrivateKey(pem);
    if (key.asymmetricKeyType !== 'ec' || key.asymmetricKeyDetails?.namedCurve !== 'prime256v1') {
      throw new Error('every signing key must be an EC P-256 key');
    }
    return pem;
  });
}

export function checkOpenssl(openssl) {
  const banner = execFileSync(openssl, ['version'], { encoding: 'utf8' }).trim();
  const match = /^OpenSSL (\d+)\.(\d+)\./.exec(banner);
  if (!match || Number(match[1]) < 3 || (Number(match[1]) === 3 && Number(match[2]) < 2)) {
    throw new Error(`need OpenSSL 3.2 or later for -sigopt nonce-type:1, ${openssl} is "${banner}"`);
  }
  return banner;
}

/** Signs twice with the deterministic (RFC 6979) nonce and refuses two different results,
    so a random nonce can never slip through. Then checks the result with Node's verifier. */
export function sign(openssl, pem, message) {
  const dir = mkdtempSync(join(tmpdir(), 'tubamp-sign-'));
  try {
    chmodSync(dir, 0o700);
    const keyFile = join(dir, 'key.pem');
    const messageFile = join(dir, 'message');
    writeFileSync(keyFile, pem, { mode: 0o600 });
    writeFileSync(messageFile, message);
    const once = () => execFileSync(openssl, ['dgst', '-sha256', '-sign', keyFile, '-sigopt', 'nonce-type:1', messageFile]);
    const der = once();
    if (!der.equals(once())) throw new Error('two signatures of the same message differ; the nonce is not deterministic');
    if (!verify('sha256', message, createPublicKey(pem), der)) throw new Error('the signature does not verify under its own key');
    return { keyId: keyIdOf(pem), der };
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

function minimumMacOSOf(dir, version) {
  const values = ARCHS.map((arch) => {
    const path = join(dir, installerName(version, arch).replace(/\.pkg$/, '.build.json'));
    return JSON.parse(readFileSync(path, 'utf8')).minimumMacOS;
  });
  if (new Set(values).size !== 1 || !values[0]) throw new Error(`the installers disagree on the minimum macOS: ${values}`);
  return values[0];
}

export function writeEnvelope({ dir, version, keysText, openssl = 'openssl' }) {
  if (!parseVersion(version)) throw new Error(`version must be X.Y.Z, got "${version}"`);
  const keys = splitPrivateKeys(keysText);
  checkOpenssl(openssl);
  const zipPath = join(dir, archiveName(version));
  const zip = readFileSync(zipPath);
  const payload = payloadFor({
    version,
    minimumMacOS: minimumMacOSOf(dir, version),
    sha256: createHash('sha256').update(zip).digest('hex'),
    size: statSync(zipPath).size,
  });
  const message = signedMessage(payload);
  const envelope = assembleEnvelope(payload, keys.map((pem) => sign(openssl, pem, message)));
  parseEnvelope(envelope);
  const out = join(dir, ENVELOPE_NAME);
  writeFileSync(`${out}.tmp`, envelope);
  renameSync(`${out}.tmp`, out);
  return { out, payload, keyIds: keys.map(keyIdOf) };
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const { values } = parseArgs({
    options: {
      version: { type: 'string' },
      dir: { type: 'string', default: 'dist' },
    },
  });
  try {
    const { out, payload, keyIds } = writeEnvelope({
      dir: values.dir,
      version: values.version ?? '',
      keysText: process.env.TUBAMP_UPDATE_SIGNING_KEYS,
      openssl: process.env.OPENSSL || 'openssl',
    });
    console.log(`envelope: ${out} signed by ${keyIds.join(', ')}\n${payload}`);
  } catch (error) {
    console.error(`envelope: ${error.message}`);
    process.exit(1);
  }
}
