#!/usr/bin/env node
// Writes SHA256SUMS and manifest.json next to the two installers. The site reads
// manifest.json from the latest GitHub Release at runtime.
//   node scripts/release/manifest.mjs --version X.Y.Z --dir dist --notes-file notes.md
// Each installer needs the tubamp-<version>-macos-<arch>.build.json that
// build-installer.sh writes beside it.
import { createHash } from 'node:crypto';
import { readFileSync, renameSync, statSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { parseArgs } from 'node:util';
import { pathToFileURL } from 'node:url';
import { parseVersion } from './conventional.mjs';

export const ARCHS = ['arm64', 'x86_64'];

export const installerName = (version, arch) => `tubamp-${version}-macos-${arch}.pkg`;

function writeAtomic(path, text) {
  writeFileSync(`${path}.tmp`, text);
  renameSync(`${path}.tmp`, path);
}

function readInstaller(dir, version, arch) {
  const name = installerName(version, arch);
  const path = join(dir, name);
  let bytes;
  try {
    bytes = readFileSync(path);
  } catch {
    throw new Error(`missing installer ${path}`);
  }
  const buildInfoPath = join(dir, name.replace(/\.pkg$/, '.build.json'));
  let build;
  try {
    build = JSON.parse(readFileSync(buildInfoPath, 'utf8'));
  } catch {
    throw new Error(`missing or unreadable build info ${buildInfoPath}`);
  }
  if (build.version !== version || build.arch !== arch) {
    throw new Error(`${buildInfoPath} describes ${build.arch} ${build.version}, expected ${arch} ${version}`);
  }
  return {
    name,
    size: statSync(path).size,
    sha256: createHash('sha256').update(bytes).digest('hex'),
    minimumMacOS: build.minimumMacOS,
  };
}

export function buildManifest({ repo, version, notes, publishedAt, installers }) {
  const minimums = new Set(ARCHS.map((arch) => installers[arch].minimumMacOS));
  if (minimums.size !== 1) throw new Error(`the installers disagree on the minimum macOS: ${[...minimums]}`);
  const tag = `v${version}`;
  const downloads = Object.fromEntries(
    ARCHS.map((arch) => {
      const { name, size, sha256 } = installers[arch];
      return [arch, { name, url: `https://github.com/${repo}/releases/download/${tag}/${name}`, size, sha256 }];
    }),
  );
  return {
    schema: 1,
    repo,
    version,
    tag,
    publishedAt,
    url: `https://github.com/${repo}/releases/tag/${tag}`,
    notes,
    downloads,
    minimumMacOS: [...minimums][0],
    signed: false,
  };
}

export function writeReleaseAssets({ dir, version, repo, notes, publishedAt }) {
  if (!parseVersion(version)) throw new Error(`version must be X.Y.Z, got "${version}"`);
  const installers = Object.fromEntries(ARCHS.map((arch) => [arch, readInstaller(dir, version, arch)]));
  const sums = ARCHS.map((arch) => `${installers[arch].sha256}  ${installers[arch].name}\n`).join('');
  const manifest = buildManifest({ repo, version, notes, publishedAt, installers });
  writeAtomic(join(dir, 'SHA256SUMS'), sums);
  writeAtomic(join(dir, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
  return manifest;
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const { values } = parseArgs({
    options: {
      version: { type: 'string' },
      dir: { type: 'string', default: 'dist' },
      'notes-file': { type: 'string' },
      repo: { type: 'string', default: process.env.GITHUB_REPOSITORY || 'IamP5/tubamp' },
      'published-at': { type: 'string' },
    },
  });
  try {
    if (!values['notes-file']) throw new Error('--notes-file is required');
    const manifest = writeReleaseAssets({
      dir: values.dir,
      version: values.version ?? '',
      repo: values.repo,
      notes: readFileSync(values['notes-file'], 'utf8'),
      publishedAt: values['published-at'] ?? new Date().toISOString().replace(/\.\d{3}Z$/, 'Z'),
    });
    console.log(`manifest: ${join(values.dir, 'manifest.json')} for ${manifest.tag}`);
  } catch (error) {
    console.error(`manifest: ${error.message}`);
    process.exit(1);
  }
}
