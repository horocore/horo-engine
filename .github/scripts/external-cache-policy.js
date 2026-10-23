'use strict';

const OWNED_PREFIX = 'horo-ci/v1/';
const DEFAULT_BUDGET_BYTES = 9 * 1000 * 1000 * 1000; // Decimal GB, matching storage billing units.

function byteCount(value, name) {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new Error(`${name} must be a non-negative safe integer`);
  }
  return value;
}

/**
 * Plan one serialized publication without deleting the last published packages.
 * Inventory includes the entire dedicated bucket, not just objects we own.
 * The caller must supply every object reachable from published manifests and
 * reserve bytes for incomplete multipart uploads before calling this function.
 * No storage mutation is performed here. A publisher must re-read inventory
 * under its single-writer lock before applying the plan.
 */
function planPublication({ inventory, protectedKeys, candidate, reservedBytes = 0,
  budgetBytes = DEFAULT_BUDGET_BYTES }) {
  byteCount(budgetBytes, 'budgetBytes');
  byteCount(reservedBytes, 'reservedBytes');
  byteCount(candidate.size, 'candidate.size');
  if (typeof candidate.key !== 'string' || !candidate.key.startsWith(OWNED_PREFIX)) {
    throw new Error('Candidate must belong to the Horo cache namespace');
  }
  const protectedSet = new Set(protectedKeys);
  const seen = new Set();
  let occupied = reservedBytes;
  for (const object of inventory) {
    if (typeof object.key !== 'string' || seen.has(object.key)) {
      throw new Error('Inventory contains an invalid or duplicate key');
    }
    seen.add(object.key);
    occupied += byteCount(object.size, 'object.size');
    byteCount(occupied, 'occupied bytes');
  }
  for (const key of protectedSet) {
    if (!seen.has(key)) throw new Error('Published manifest references a missing object');
  }
  if (seen.has(candidate.key)) {
    throw new Error('Publication keys are immutable; verify existing content separately');
  }
  const peakBytes = occupied + candidate.size;
  byteCount(peakBytes, 'peak bytes');
  const eligible = inventory.filter(object => object.key.startsWith(OWNED_PREFIX)
    && !protectedSet.has(object.key) && Number.isFinite(Date.parse(object.modifiedAt)))
    .sort((a, b) => Date.parse(a.modifiedAt) - Date.parse(b.modifiedAt)
      || a.key.localeCompare(b.key));
  let projectedBytes = peakBytes;
  const deletions = [];
  for (const object of eligible) {
    if (projectedBytes <= budgetBytes) break;
    deletions.push(object.key);
    projectedBytes -= object.size;
  }
  if (projectedBytes > budgetBytes) {
    // Do not evict anything for a publication that cannot fit anyway.
    return { allowed: false, reason: 'capacity', deletions: [], peakBytes,
      projectedBytes: peakBytes };
  }
  return { allowed: true, reason: 'fits', deletions, peakBytes, projectedBytes };
}

module.exports = { DEFAULT_BUDGET_BYTES, planPublication };
