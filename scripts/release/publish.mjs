#!/usr/bin/env node
// Publishes the GitHub Release for one version, converging from whatever an earlier run
// left behind: no release, a draft with some assets, or a published release.
//   node scripts/release/publish.mjs --version X.Y.Z --sha <commit> --dir dist --notes-file notes.md [--dry-run]
// The release stays a draft until all four assets are uploaded, manifest.json last, so
// releases/latest/download/manifest.json never names an installer that is not there.
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { parseArgs } from 'node:util';
import { ARCHS, installerName } from './manifest.mjs';
import { parseVersion } from './conventional.mjs';

const { values } = parseArgs({
  options: {
    version: { type: 'string' },
    sha: { type: 'string' },
    dir: { type: 'string', default: 'dist' },
    'notes-file': { type: 'string' },
    repo: { type: 'string', default: process.env.GITHUB_REPOSITORY || 'IamP5/tubamp' },
    'dry-run': { type: 'boolean', default: false },
  },
});

const { version, sha, dir, repo } = values;
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

const installers = ARCHS.map((arch) => installerName(version, arch));
const manifestName = 'manifest.json';
const assetNames = [...installers, 'SHA256SUMS', manifestName];
for (const name of assetNames) {
  if (!existsSync(join(dir, name))) fail(`missing asset ${join(dir, name)}`);
}
const manifest = JSON.parse(readFileSync(join(dir, manifestName), 'utf8'));
if (manifest.version !== version) fail(`${manifestName} is for ${manifest.version}, expected ${version}`);

checkTag();

let release = findRelease();
// A draft has no tag yet, so a draft another run left for a different commit is safe to
// delete. Retargeting it would keep that commit's installers.
if (release?.draft && release.target !== sha) {
  console.log(`${tag} has a draft for ${release.target}, not ${sha}; replacing it with a fresh draft`);
  mutate('api', '-X', 'DELETE', `repos/${repo}/releases/${release.id}`);
  release = null;
}
if (!release) {
  mutate('release', 'create', tag, '--draft', '--target', sha, '--title', `tubamp ${version}`, '--notes-file', notesFile);
  release = dryRun ? { draft: true, assets: [] } : findRelease();
  if (!release) fail(`created a draft for ${tag} but cannot find it`);
}

const uploaded = new Set(release.assets.filter((a) => a.state === 'uploaded').map((a) => a.name));
const missing = assetNames.filter((name) => !uploaded.has(name));

if (!release.draft && missing.length === 0) {
  console.log(`${tag} is already published with all assets; nothing to do`);
  process.exit(0);
}

if (missing.length > 0) {
  console.log(`${tag} is missing ${missing.join(', ')}; uploading the full set so the checksums match`);
  const paths = (names) => names.map((name) => join(dir, name));
  mutate('release', 'upload', tag, ...paths([...installers, 'SHA256SUMS']), '--clobber');
  mutate('release', 'upload', tag, ...paths([manifestName]), '--clobber');
}

if (release.draft) {
  checkTag();
  mutate('release', 'edit', tag, '--draft=false', '--latest');
}

if (!dryRun) {
  const final = findRelease();
  const finalUploaded = new Set(final.assets.filter((a) => a.state === 'uploaded').map((a) => a.name));
  const stillMissing = assetNames.filter((name) => !finalUploaded.has(name));
  if (final.draft || stillMissing.length) fail(`${tag} did not converge: draft=${final.draft} missing=${stillMissing}`);
  if (checkTag() !== sha) fail(`${tag} was published but the tag does not point at ${sha}`);
}
console.log(`${dryRun ? 'dry run: would publish' : 'published'} ${tag} at ${sha} with ${assetNames.join(', ')}`);
