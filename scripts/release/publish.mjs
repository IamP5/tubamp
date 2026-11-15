#!/usr/bin/env node
// Publishes the GitHub Release for one version, converging from whatever an earlier run
// left behind: no release, a draft with some assets, or a published release.
//   node scripts/release/publish.mjs --version X.Y.Z --sha <commit> --dir dist --notes-file notes.md
//                                    --updates on|off [--dry-run]
// --updates is the plan's decision. Off publishes the two installers, SHA256SUMS and
// manifest.json. On adds the universal update archive and update-v1.signed. The script
// refuses a half-armed release: an installer built with the other setting, an update
// asset in an off release, a missing one in an on release, or an envelope that names
// another archive.
// The release stays a draft until every asset is uploaded, in this order: the installers,
// the archive and SHA256SUMS; then update-v1.signed, after the archive it names; then
// manifest.json. So releases/latest/download/ never serves a file whose partner is missing.
// An archive and envelope already uploaded to the release, and valid for it, are kept and
// copied into --dir in place of this run's pair: `ditto -c -k` is not reproducible, and an
// installed copy may hold the uploaded envelope, so the archive it names never changes.
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { copyFileSync, existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { parseArgs } from 'node:util';
import { ARCHS, installerName } from './manifest.mjs';
import { ENVELOPE_NAME, PRODUCT, archiveName, parseEnvelope } from './envelope.mjs';
import { parseVersion } from './conventional.mjs';

const { values } = parseArgs({
  options: {
    version: { type: 'string' },
    sha: { type: 'string' },
    dir: { type: 'string', default: 'dist' },
    'notes-file': { type: 'string' },
    updates: { type: 'string' },
    repo: { type: 'string', default: process.env.GITHUB_REPOSITORY || 'IamP5/tubamp' },
    'dry-run': { type: 'boolean', default: false },
  },
});

const { version, sha, dir, repo, updates } = values;
const dryRun = values['dry-run'];
const notesFile = values['notes-file'];
const tag = `v${version}`;

function fail(message) {
  console.error(`publish: ${message}`);
  process.exit(1);
}

const gh = (...args) => execFileSync('gh', args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'inherit'] });

function mutate(...args) {
  console.log(`${dryRun ? 'would run' : 'run'}: gh ${args.join(' ')}`);
  if (!dryRun) gh(...args);
}

function remoteTagCommit() {
  const refs = JSON.parse(gh('api', `repos/${repo}/git/matching-refs/tags/${tag}`));
  const ref = refs.find((r) => r.ref === `refs/tags/${tag}`);
  if (!ref) return null;
  if (ref.object.type === 'tag') {
    return JSON.parse(gh('api', `repos/${repo}/git/tags/${ref.object.sha}`)).object.sha;
  }
  return ref.object.sha;
}

function checkTag() {
  const commit = remoteTagCommit();
  if (commit && commit !== sha) fail(`${tag} already points at ${commit}, not ${sha}; refusing to publish over it`);
  return commit;
}

function findRelease() {
  const out = gh(
    'api',
    '--paginate',
    `repos/${repo}/releases?per_page=100`,
    '--jq',
    `.[] | select(.tag_name == "${tag}") | {id, draft, target: .target_commitish, assets: [.assets[] | {name, state}]}`,
  );
  const found = out.split('\n').filter(Boolean).map((line) => JSON.parse(line));
  if (found.length > 1) fail(`${found.length} releases carry the tag ${tag}; delete the extra drafts by hand`);
  return found[0] ?? null;
}

// GitHub's release list can lag a create by a second or two: v0.1.0's first publish
// looked in the same second and stopped with the draft already there.
const LOOKUP_WAITS_MS = [0, 500, 1000, 2000, 4000, 8000];

function findNewDraft() {
  for (const wait of LOOKUP_WAITS_MS) {
    if (wait) Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, wait);
    const found = findRelease();
    if (found) return found;
  }
  return null;
}

if (!parseVersion(version ?? '')) fail(`--version must be X.Y.Z, got "${version}"`);
if (!/^[0-9a-f]{40}$/.test(sha ?? '')) fail(`--sha must be a full commit SHA, got "${sha}"`);
if (!notesFile || !existsSync(notesFile)) fail('--notes-file must name an existing file');
if (updates !== 'on' && updates !== 'off') fail(`--updates must be on or off, got "${updates}"`);

/** Why the envelope bytes do not describe the archive bytes for this release, or null. */
function envelopeProblem(envelopeBytes, zip) {
  let envelope;
  try {
    envelope = parseEnvelope(envelopeBytes).fields;
  } catch (error) {
    return error.message;
  }
  if (envelope.product !== PRODUCT || envelope.version !== version) {
    return `${ENVELOPE_NAME} is for ${envelope.product} ${envelope.version}, expected ${PRODUCT} ${version}`;
  }
  const sha256 = createHash('sha256').update(zip).digest('hex');
  if (envelope.archive?.sha256 !== sha256 || envelope.archive?.size !== zip.length) {
    return `${ENVELOPE_NAME} names an archive of ${envelope.archive?.size} bytes with SHA-256 ${envelope.archive?.sha256}, not ${archive}`;
  }
  return null;
}

const installers = ARCHS.map((arch) => installerName(version, arch));
const archive = archiveName(version);
const manifestName = 'manifest.json';
const updateAssets = [archive, ENVELOPE_NAME];
const uploadGroups = updates === 'on'
  ? [[...installers, archive, 'SHA256SUMS'], [ENVELOPE_NAME], [manifestName]]
  : [[...installers, 'SHA256SUMS'], [manifestName]];
const assetNames = uploadGroups.flat();

for (const name of assetNames) {
  if (!existsSync(join(dir, name))) fail(`missing asset ${join(dir, name)}${updateAssets.includes(name) ? ' (--updates on)' : ''}`);
}
const manifest = JSON.parse(readFileSync(join(dir, manifestName), 'utf8'));
if (manifest.version !== version) fail(`${manifestName} is for ${manifest.version}, expected ${version}`);

for (const name of installers) {
  const buildInfo = join(dir, name.replace(/\.pkg$/, '.build.json'));
  const built = existsSync(buildInfo) ? JSON.parse(readFileSync(buildInfo, 'utf8')).updates : undefined;
  if (built !== (updates === 'on')) {
    fail(`${name} was built with updates ${built === true ? 'on' : built === false ? 'off' : 'unknown'} but this release has --updates ${updates}; refusing a half-armed release`);
  }
}
if (updates === 'off') {
  const stray = updateAssets.filter((name) => existsSync(join(dir, name)));
  if (stray.length) fail(`${stray.join(', ')} in ${dir} but this release has --updates off; refusing a half-armed release`);
} else {
  const problem = envelopeProblem(readFileSync(join(dir, ENVELOPE_NAME)), readFileSync(join(dir, archive)));
  if (problem) fail(problem);
}

checkTag();

let release = findRelease();
const uploadedNames = (r) => r.assets.filter((a) => a.state === 'uploaded').map((a) => a.name);
const foreign = (r) => uploadedNames(r).filter((name) => !assetNames.includes(name));

// A published release with an asset this run would not upload was armed differently.
// Removing an asset from a live release would strand clients, so stop instead.
if (release && !release.draft && foreign(release).length) {
  fail(`${tag} is published with ${foreign(release).join(', ')}, which a release with --updates ${updates} does not carry; refusing a half-armed release`);
}
// A draft has no tag yet, so a draft another run left is safe to delete: one for another
// commit, whose installers are not this commit's, or one armed differently.
if (release?.draft && (release.target !== sha || foreign(release).length)) {
  const why = release.target !== sha ? `for ${release.target}, not ${sha}` : `with ${foreign(release).join(', ')}`;
  console.log(`${tag} has a draft ${why}; replacing it with a fresh draft`);
  mutate('api', '-X', 'DELETE', `repos/${repo}/releases/${release.id}`);
  release = null;
}
if (!release) {
  mutate('release', 'create', tag, '--draft', '--target', sha, '--title', `tubamp ${version}`, '--notes-file', notesFile);
  release = dryRun ? { draft: true, assets: [] } : findNewDraft();
  if (!release) fail(`created a draft for ${tag} but cannot find it`);
}

/** Keeps the release's uploaded archive and envelope when they are valid for it, copying
    them over this run's pair in --dir. A published release whose envelope does not name
    an uploaded archive stops the run: no archive can be uploaded under that envelope. */
function keepUploadedUpdatePair(r) {
  const names = uploadedNames(r);
  if (!names.includes(ENVELOPE_NAME)) return false;
  const scratch = mkdtempSync(join(tmpdir(), 'tubamp-publish-'));
  try {
    const present = [archive, ENVELOPE_NAME].filter((name) => names.includes(name));
    gh('release', 'download', tag, ...present.flatMap((name) => ['--pattern', name]), '--dir', scratch);
    const problem = names.includes(archive)
      ? envelopeProblem(readFileSync(join(scratch, ENVELOPE_NAME)), readFileSync(join(scratch, archive)))
      : `${archive} is missing`;
    if (problem && !r.draft) {
      fail(`${tag} is published with an ${ENVELOPE_NAME} that installed copies may hold, and ${problem}. `
        + 'A rebuilt archive would not match it; fix forward with a higher version');
    }
    if (problem) {
      console.log(`${tag} has a draft whose update assets do not match (${problem}); uploading this run's pair`);
      return false;
    }
    for (const name of [archive, ENVELOPE_NAME]) copyFileSync(join(scratch, name), join(dir, name));
    console.log(`${tag} already holds a valid ${archive} and ${ENVELOPE_NAME}; keeping them and using them in ${dir}`);
    return true;
  } finally {
    rmSync(scratch, { recursive: true, force: true });
  }
}

const kept = updates === 'on' && keepUploadedUpdatePair(release) ? [archive, ENVELOPE_NAME] : [];
const uploaded = new Set(uploadedNames(release));
const missing = assetNames.filter((name) => !uploaded.has(name));

if (!release.draft && missing.length === 0) {
  console.log(`${tag} is already published with all assets; nothing to do`);
  process.exit(0);
}

if (missing.length > 0) {
  console.log(`${tag} is missing ${missing.join(', ')}; uploading the full set so the checksums match`);
  for (const group of uploadGroups.map((g) => g.filter((name) => !kept.includes(name))).filter((g) => g.length)) {
    mutate('release', 'upload', tag, ...group.map((name) => join(dir, name)), '--clobber');
  }
}

if (release.draft) {
  checkTag();
  mutate('release', 'edit', tag, '--draft=false', '--latest');
}

if (!dryRun) {
  const final = findRelease();
  const finalUploaded = new Set(uploadedNames(final));
  const stillMissing = assetNames.filter((name) => !finalUploaded.has(name));
  if (final.draft || stillMissing.length) fail(`${tag} did not converge: draft=${final.draft} missing=${stillMissing}`);
  if (checkTag() !== sha) fail(`${tag} was published but the tag does not point at ${sha}`);
}
console.log(`${dryRun ? 'dry run: would publish' : 'published'} ${tag} at ${sha} with updates ${updates}: ${assetNames.join(', ')}`);
