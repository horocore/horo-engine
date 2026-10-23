'use strict';

const assert = require('node:assert/strict');
const { test } = require('node:test');
const { supersededSnapshots, retainCompilerCaches } = require('../../.github/scripts/retain_compiler_caches.js');

const snapshot = (id, overrides = {}) => ({
  id, key: `sonar-v2-gcc-Linux-X64-main-${id}-1`, version: 'archive-v1',
  ref: 'refs/heads/main', size_in_bytes: 100,
  created_at: `2026-10-23T12:00:0${id}Z`, ...overrides,
});

test('retain newest compatible main snapshot and protect other cache owners', () => {
  const caches = [snapshot(1), snapshot(2),
    snapshot(3, { ref: 'refs/pull/123/merge' }),
    snapshot(4, { key: 'sonar-v4-deps-Linux-X64-hash' }),
    snapshot(5, { version: 'archive-v2' }),
    snapshot(6, { key: 'unrelated-main-6-1' }),
    snapshot(7, { size_in_bytes: 0 }),
    snapshot(8, { created_at: 'invalid' })];
  assert.deepEqual(supersededSnapshots(caches).map(item => [item.cache.id, item.replacement.id]), [[1, 2]]);
});

test('native platforms and toolchain profiles cannot retire each other', () => {
  const native = (id, platform, compiler) => snapshot(id, {
    key: `native-v1-${platform}-X64-${compiler}-${compiler}-native-tests-v1-Debug-main-${id}-1`,
  });
  const caches = [native(1, 'Windows', 'msvc-19.44'), native(2, 'Windows', 'msvc-19.44'),
    native(3, 'Linux', 'gcc-13'), native(4, 'Windows', 'msvc-20')];
  assert.deepEqual(supersededSnapshots(caches).map(item => item.cache.id), [1]);
});

function client(inventories, deletes) {
  return {
    paginate: async () => inventories.length > 1 ? inventories.shift() : inventories[0],
    rest: { actions: {
      getActionsCacheList: {},
      deleteActionsCacheById: async request => deletes.push(request.cache_id),
    } },
  };
}

test('dry run does not delete and live run rechecks eviction before deleting', async () => {
  const deletes = [];
  const args = { context: { repo: { owner: 'horocore', repo: 'horo-engine' } }, core: { info() {} } };
  await retainCompilerCaches({ ...args, github: client([[snapshot(1), snapshot(2)]], deletes) });
  assert.deepEqual(deletes, []);
  await retainCompilerCaches({ ...args, dryRun: false,
    github: client([[snapshot(1), snapshot(2)], [snapshot(1)]], deletes) });
  assert.deepEqual(deletes, []);
  await retainCompilerCaches({ ...args, dryRun: false,
    github: client([[snapshot(1), snapshot(2)]], deletes) });
  assert.deepEqual(deletes, [1]);
});

test('a replacement uploaded during maintenance postpones deletion until the next inventory', async () => {
  const deletes = [];
  await retainCompilerCaches({
    github: client([[snapshot(1), snapshot(2)], [snapshot(1), snapshot(2), snapshot(3)]], deletes),
    context: { repo: { owner: 'horocore', repo: 'horo-engine' } },
    core: { info() {} }, dryRun: false,
  });
  assert.deepEqual(deletes, []);
});

test('API failures propagate; an already evicted entry is harmless', async () => {
  for (const status of [404, 403]) {
    const github = client([[snapshot(1), snapshot(2)]], []);
    github.rest.actions.deleteActionsCacheById = async () => { throw Object.assign(new Error('API failure'), { status }); };
    const request = retainCompilerCaches({ github,
      context: { repo: { owner: 'horocore', repo: 'horo-engine' } },
      core: { info() {} }, dryRun: false });
    if (status === 404) await request;
    else await assert.rejects(request, { status: 403 });
  }
});


test('auxiliary generations retire only their own superseded checkpoints', () => {
  const key = 'native-v1-Linux-X64-gcc-13-gcc-13-native-variants-v1-Mixed-main-';
  const primary = 'native-v1-Linux-X64-gcc-13-gcc-13-native-tests-v1-Debug-main-';
  const caches = [snapshot(1, { key: `${key}1-1` }), snapshot(2, { key: `${key}2-1` }),
    snapshot(3, { key: `${primary}3-1` })];
  assert.deepEqual(supersededSnapshots(caches).map(item => [item.cache.id, item.replacement.id]), [[1, 2]]);
});
