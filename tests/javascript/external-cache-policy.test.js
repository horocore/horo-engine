'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { planPublication } = require('../../.github/scripts/external-cache-policy');

const object = (name, size, modifiedAt = '2026-10-20T00:00:00Z') => ({
  key: `horo-ci/v1/${name}`, size, modifiedAt,
});
const candidate = object('next', 30);
const plan = (inventory, protectedKeys = [], overrides = {}) => planPublication({
  inventory, protectedKeys, candidate, budgetBytes: 100, ...overrides,
});

test('counts upload overlap before the manifest can switch to the new package', () => {
  const current = object('current', 80);
  assert.equal(plan([current], [current.key]).allowed, false);
});

test('evicts only enough unreferenced generations, oldest first', () => {
  const current = object('current', 40);
  const old = object('old', 20);
  const recent = object('recent', 20, '2026-10-21T00:00:00Z');
  const result = plan([recent, current, old], [current.key]);
  assert.deepEqual(result.deletions, [old.key]);
  assert.equal(result.projectedBytes, 90);
});

test('never proposes destructive cleanup when the candidate cannot fit', () => {
  const current = object('current', 90);
  const result = plan([current, object('old', 20)], [current.key]);
  assert.equal(result.allowed, false);
  assert.deepEqual(result.deletions, []);
});

test('counts foreign objects but never deletes them', () => {
  const result = plan([{ ...object('foreign', 80), key: 'unrelated/data' }]);
  assert.equal(result.allowed, false);
  assert.deepEqual(result.deletions, []);
});

test('reserves incomplete upload bytes and rejects invalid accounting', () => {
  assert.equal(plan([], [], { reservedBytes: 80 }).allowed, false);
  for (const size of [-1, NaN, Infinity, 1.5, Number.MAX_SAFE_INTEGER + 1]) {
    assert.throws(() => plan([object('bad', size)]));
  }
});

test('rejects incomplete inventory and immutable key replacement', () => {
  assert.throws(() => plan([], ['horo-ci/v1/missing']));
  assert.throws(() => plan([candidate]));
  assert.throws(() => plan([object('duplicate', 1), object('duplicate', 1)]));
});

test('unknown modification times are preserved', () => {
  assert.equal(plan([object('unknown', 80, 'invalid')]).allowed, false);
});

test('a fitting publication does not prune extra generations', () => {
  const result = plan([object('old', 50)]);
  assert.equal(result.allowed, true);
  assert.deepEqual(result.deletions, []);
  assert.equal(result.projectedBytes, 80);
});
