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
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';
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

if (!parseVersion(version ?? '')) fail(`--version must be X.Y.Z, got "${version}"`);
if (!/^[0-9a-f]{40}$/.test(sha ?? '')) fail(`--sha must be a full commit SHA, got "${sha}"`);
if (!notesFile || !existsSync(notesFile)) fail('--notes-file must name an existing file');
if (updates !== 'on' && updates !== 'off') fail(`--updates must be on or off, got "${updates}"`);

const installers = ARCHS.map((arch) => installerName(version, arch));
const archive = archiveName(version);
const manifestName = 'manifest.json';
const updateAssets = [archive, ENVELOPE_NAME];
// Each group is uploaded after the one before it has finished.
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
  let envelope;
  try {
    envelope = parseEnvelope(readFileSync(join(dir, ENVELOPE_NAME))).fields;
  } catch (error) {
    fail(error.message);
  }
  const zip = readFileSync(join(dir, archive));
  const sha256 = createHash('sha256').update(zip).digest('hex');
  if (envelope.product !== PRODUCT || envelope.version !== version) {
    fail(`${ENVELOPE_NAME} is for ${envelope.product} ${envelope.version}, expected ${PRODUCT} ${version}`);
  }
  if (envelope.archive?.sha256 !== sha256 || envelope.archive?.size !== zip.length) {
    fail(`${ENVELOPE_NAME} names an archive of ${envelope.archive?.size} bytes with SHA-256 ${envelope.archive?.sha256}, not ${archive}`);
  }
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
  release = dryRun ? { draft: true, assets: [] } : findRelease();
  if (!release) fail(`created a draft for ${tag} but cannot find it`);
}

const uploaded = new Set(uploadedNames(release));
const missing = assetNames.filter((name) => !uploaded.has(name));

if (!release.draft && missing.length === 0) {
  console.log(`${tag} is already published with all assets; nothing to do`);
  process.exit(0);
}

if (missing.length > 0) {
  console.log(`${tag} is missing ${missing.join(', ')}; uploading the full set so the checksums match`);
  for (const group of uploadGroups) {
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
