import { test } from 'node:test';
import assert from 'node:assert/strict';
import { runScript } from './helpers.mjs';

const lint = (title) => runScript('lint-title.mjs', { env: { PR_TITLE: title } });

test('conventional titles pass and say what they release', () => {
  assert.match(lint('feat(reverb): add a plate type').stdout, /minor release/);
  assert.match(lint('fix: stop the gate chattering').stdout, /patch release/);
  assert.match(lint('perf: faster A2').stdout, /patch release/);
  assert.match(lint('docs: explain presets').stdout, /no release/);
  assert.match(lint('refactor(dsp)!: drop classic engines').stdout, /breaking/);
  for (const title of ['ci: x', 'chore: x', 'test: x', 'build: x', 'style: x']) {
    assert.equal(lint(title).code, 0, title);
  }
});

test('other titles fail with the allowed types', () => {
  for (const title of ['ui: redesign the board', 'Add a plate reverb', 'feat:no space', 'Feat: capital', 'feat: ', '', 'feat(): empty scope']) {
    const result = lint(title);
    assert.equal(result.code, 1, title);
    assert.match(result.stderr, /feat, fix, perf/);
  }
});
