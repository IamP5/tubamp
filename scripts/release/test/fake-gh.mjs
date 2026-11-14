#!/usr/bin/env node
// Stands in for the gh CLI: keeps GitHub's tags and releases in the JSON file named by
// FAKE_GH_STATE. FAKE_GH_CRASH_ON=<asset name> makes that asset's upload die first.
import { readFileSync, writeFileSync } from 'node:fs';
import { basename } from 'node:path';

const statePath = process.env.FAKE_GH_STATE;
const state = JSON.parse(readFileSync(statePath, 'utf8'));
const args = process.argv.slice(2);
const save = () => writeFileSync(statePath, JSON.stringify(state, null, 2));
const byTag = (tag) => state.releases.find((r) => r.tag_name === tag);

state.calls.push(args);
save();

if (args[0] === 'api') {
  const path = args.find((a) => a.startsWith('repos/'));
  const matching = /\/git\/matching-refs\/tags\/(.+)$/.exec(path);
  if (matching) {
    const refs = Object.entries(state.tags)
      .filter(([name]) => name.startsWith(matching[1]))
      .map(([name, sha]) => ({ ref: `refs/tags/${name}`, object: { type: 'commit', sha } }));
    process.stdout.write(JSON.stringify(refs));
  } else if (path.includes('/releases?')) {
    const tag = /tag_name == "([^"]+)"/.exec(args[args.indexOf('--jq') + 1])[1];
    for (const r of state.releases.filter((x) => x.tag_name === tag)) {
      process.stdout.write(`${JSON.stringify({ id: r.id, draft: r.draft, assets: r.assets })}\n`);
    }
  } else {
    process.stderr.write(`fake gh: unhandled api ${path}\n`);
    process.exit(1);
  }
} else if (args[0] === 'release' && args[1] === 'create') {
  const tag = args[2];
  if (!args.includes('--draft')) process.exit(1);
  state.releases.push({ id: state.releases.length + 1, tag_name: tag, draft: true, target: args[args.indexOf('--target') + 1], assets: [] });
  save();
} else if (args[0] === 'release' && args[1] === 'upload') {
  const release = byTag(args[2]);
  if (!release) process.exit(1);
  for (const file of args.slice(3).filter((a) => !a.startsWith('--'))) {
    const name = basename(file);
    if (name === process.env.FAKE_GH_CRASH_ON) {
      process.stderr.write(`fake gh: crashed while uploading ${name}\n`);
      process.exit(1);
    }
    release.assets = release.assets.filter((a) => a.name !== name);
    release.assets.push({ name, state: 'uploaded' });
    save();
  }
} else if (args[0] === 'release' && args[1] === 'edit') {
  const release = byTag(args[2]);
  if (!release) process.exit(1);
  if (args.includes('--draft=false')) {
    release.draft = false;
    state.tags[release.tag_name] ??= release.target;
    state.latest = release.tag_name;
  }
  save();
} else {
  process.stderr.write(`fake gh: unhandled ${args.join(' ')}\n`);
  process.exit(1);
}
