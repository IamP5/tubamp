#!/usr/bin/env node
// Release notes for the version plan.mjs computes, from the same commit range.
//   node scripts/release/notes.mjs [--out notes.md]
import { writeFileSync } from 'node:fs';
import { parseArgs } from 'node:util';
import { pathToFileURL } from 'node:url';
import { planRelease } from './plan.mjs';

const SECTIONS = [
  ['Breaking changes', (c) => c.breaking],
  ['Features', (c) => !c.breaking && c.type === 'feat'],
  ['Fixes', (c) => !c.breaking && c.type === 'fix'],
  ['Performance', (c) => !c.breaking && c.type === 'perf'],
];

function entry(commit, repo) {
  const scope = commit.scope ? `**${commit.scope}:** ` : '';
  const link = `[${commit.sha.slice(0, 7)}](https://github.com/${repo}/commit/${commit.sha})`;
  const note = commit.breakingNote ? `\n  ${commit.breakingNote}` : '';
  return `- ${scope}${commit.description} (${link})${note}`;
}

export function renderNotes(plan, { repo }) {
  if (!plan.release) return `Nothing to release: ${plan.reason}.\n`;
  const parts = [];
  for (const [title, belongs] of SECTIONS) {
    const lines = plan.commits.filter(belongs).map((c) => entry(c, repo));
    if (lines.length) parts.push(`## ${title}\n\n${lines.join('\n')}`);
  }
  if (!parts.length) parts.push(`Maintenance release (${plan.reason}).`);
  parts.push(
    '## Install\n\n' +
      `Download \`tubamp-${plan.version}-macos-arm64.pkg\` for Apple Silicon Macs or ` +
      `\`tubamp-${plan.version}-macos-x86_64.pkg\` for Intel Macs, then open it. ` +
      'The installer is not signed with an Apple Developer ID yet: if macOS blocks it, ' +
      'open System Settings, Privacy & Security, and choose Open Anyway. ' +
      '`SHA256SUMS` lists the checksum of each installer.',
  );
  return `${parts.join('\n\n')}\n`;
}

if (import.meta.url === pathToFileURL(process.argv[1]).href) {
  const { values } = parseArgs({
    options: {
      out: { type: 'string' },
      repo: { type: 'string', default: process.env.GITHUB_REPOSITORY || 'IamP5/tubamp' },
    },
  });
  try {
    const notes = renderNotes(planRelease(), { repo: values.repo });
    if (values.out) writeFileSync(values.out, notes);
    else process.stdout.write(notes);
  } catch (error) {
    console.error(`notes: ${error.message}`);
    process.exit(1);
  }
}
