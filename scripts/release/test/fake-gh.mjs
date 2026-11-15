#!/usr/bin/env node
// Stands in for the gh CLI: keeps GitHub's tags and releases, asset bytes included, in the
// JSON file named by FAKE_GH_STATE. FAKE_GH_CRASH_ON=<asset name> makes that asset's
// upload die first. FAKE_GH_LIST_LAG=<n> hides a release from the first n listings after
// it is created, as GitHub's own list sometimes does.
import { readFileSync, writeFileSync } from 'node:fs';
import { basename, join } from 'node:path';

const statePath = process.env.FAKE_GH_STATE;
const state = JSON.parse(readFileSync(statePath, 'utf8'));
const args = process.argv.slice(2);
const save = () => writeFileSync(statePath, JSON.stringify(state, null, 2));
const byTag = (tag) => state.releases.find((r) => r.tag_name === tag);

state.calls.push(args);
save();

if (args[0] === 'api' && args.includes('DELETE')) {
  const id = Number(/\/releases\/(\d+)$/.exec(args.find((a) => a.startsWith('repos/')))[1]);
  state.releases = state.releases.filter((r) => r.id !== id);
  save();
} else if (args[0] === 'api') {
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
      if (r.hiddenFor > 0) {
        r.hiddenFor -= 1;
        save();
        continue;
      }
      const assets = r.assets.map(({ name, state }) => ({ name, state }));
      process.stdout.write(`${JSON.stringify({ id: r.id, draft: r.draft, target: r.target, assets })}\n`);
    }
  } else {
    process.stderr.write(`fake gh: unhandled api ${path}\n`);
    process.exit(1);
  }
} else if (args[0] === 'release' && args[1] === 'create') {
  const tag = args[2];
  if (!args.includes('--draft')) process.exit(1);
  state.releases.push({ id: Math.max(0, ...state.releases.map((r) => r.id)) + 1, tag_name: tag, draft: true, target: args[args.indexOf('--target') + 1], assets: [], hiddenFor: Number(process.env.FAKE_GH_LIST_LAG ?? 0) });
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
    release.assets.push({ name, state: 'uploaded', content: readFileSync(file).toString('base64') });
    save();
  }
} else if (args[0] === 'release' && args[1] === 'download') {
  const release = byTag(args[2]);
  if (!release) process.exit(1);
  const dir = args[args.indexOf('--dir') + 1];
  const patterns = args.flatMap((a, i) => (args[i - 1] === '--pattern' ? [a] : []));
  for (const name of patterns) {
    const asset = release.assets.find((a) => a.name === name && a.state === 'uploaded');
    if (!asset) {
      process.stderr.write(`fake gh: no asset ${name}\n`);
      process.exit(1);
    }
    writeFileSync(join(dir, name), Buffer.from(asset.content ?? '', 'base64'));
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
