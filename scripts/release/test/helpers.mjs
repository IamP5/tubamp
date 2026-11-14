import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdirSync, mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const scriptsDir = dirname(dirname(fileURLToPath(import.meta.url)));

export function tempDir(prefix = 'tubamp-release-') {
  return mkdtempSync(join(tmpdir(), prefix));
}

export function makeRepo() {
  const dir = tempDir('tubamp-release-repo-');
  const git = (...args) => execFileSync('git', args, { cwd: dir, encoding: 'utf8' }).trim();
  git('init', '-q', '-b', 'main');
  git('config', 'user.email', 'test@example.invalid');
  git('config', 'user.name', 'test');
  git('config', 'commit.gpgsign', 'false');
  git('config', 'tag.gpgsign', 'false');
  let n = 0;
  return {
    dir,
    git,
    commit(message, files = [`f${n}.txt`]) {
      n++;
      for (const file of files) {
        mkdirSync(dirname(join(dir, file)), { recursive: true });
        writeFileSync(join(dir, file), `${file} ${n}`);
        git('add', file);
      }
      git('commit', '-q', '--allow-empty', '-m', message);
      return git('rev-parse', 'HEAD');
    },
    tag(name, ref = 'HEAD') {
      git('tag', name, ref);
    },
  };
}

export function runScript(name, { args = [], cwd, env = {} } = {}) {
  const result = spawnSync(process.execPath, [join(scriptsDir, name), ...args], {
    cwd,
    encoding: 'utf8',
    env: { PATH: process.env.PATH, HOME: process.env.HOME, ...env },
  });
  return { code: result.status, stdout: result.stdout, stderr: result.stderr };
}

export function plan(repo, args = []) {
  const result = runScript('plan.mjs', { cwd: repo.dir, args });
  if (result.code !== 0) return { error: result.stderr, code: result.code };
  return JSON.parse(result.stdout);
}

export function fakeInstallers(version, { minimumMacOS = '11.0' } = {}) {
  const dir = tempDir('tubamp-release-dist-');
  const files = {};
  for (const arch of ['arm64', 'x86_64']) {
    const name = `tubamp-${version}-macos-${arch}.pkg`;
    const bytes = Buffer.from(`pkg for ${arch} `.repeat(arch === 'arm64' ? 3 : 5));
    writeFileSync(join(dir, name), bytes);
    writeFileSync(
      join(dir, `tubamp-${version}-macos-${arch}.build.json`),
      JSON.stringify({ version, arch, minimumMacOS }),
    );
    files[arch] = { name, size: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex') };
  }
  writeFileSync(join(dir, 'notes.md'), '## Features\n\n- a plate reverb\n');
  return { dir, files };
}
