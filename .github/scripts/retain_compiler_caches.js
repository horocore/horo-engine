'use strict';

// Only immutable compiler snapshots owned by these workflows are eligible.
// Source, scanner, other workflow and PR caches are never deleted here.
function snapshotPrefix(cache) {
  if (cache.ref !== 'refs/heads/main' || cache.size_in_bytes <= 0) return null;
  const match = cache.key.match(/^(native-v1-(?:Linux|Windows|macOS)-(?:X64|ARM64)-.+-native-tests-v1-Debug-main-|sonar-v2-gcc-Linux-X64-main-)\d+-\d+$/);
  return match ? `${match[1]}:${cache.version}` : null;
}

function supersededSnapshots(caches) {
  const groups = new Map();
  for (const cache of caches) {
    const prefix = snapshotPrefix(cache);
    if (!prefix || !Number.isFinite(Date.parse(cache.created_at))) continue;
    if (!groups.has(prefix)) groups.set(prefix, []);
    groups.get(prefix).push(cache);
  }
  return [...groups.values()].flatMap(group => {
    group.sort((a, b) => Date.parse(b.created_at) - Date.parse(a.created_at) || b.id - a.id);
    return group.slice(1).map(cache => ({ cache, replacement: group[0] }));
  });
}

async function retainCompilerCaches({ github, context, core, dryRun = true }) {
  const repository = context.repo;
  const list = () => github.paginate(github.rest.actions.getActionsCacheList, {
    ...repository, per_page: 100,
  });
  const inventory = await list();
  const bytes = inventory.reduce((sum, cache) => sum + cache.size_in_bytes, 0);
  core.info(`Repository cache inventory: ${inventory.length} entries, ${bytes} bytes`);
  for (const { cache, replacement } of supersededSnapshots(inventory)) {
    core.info(`${dryRun ? 'Would retire' : 'Retiring'} ${cache.key}; retain ${replacement.key}`);
    if (dryRun) continue;
    // A concurrent upload or eviction can invalidate the inventory. Require
    // both entries and the same replacement immediately before each deletion.
    const fresh = supersededSnapshots(await list());
    if (!fresh.some(item => item.cache.id === cache.id && item.replacement.id === replacement.id)) continue;
    try {
      await github.rest.actions.deleteActionsCacheById({ ...repository, cache_id: cache.id });
    } catch (error) {
      if (error.status !== 404) throw error;
    }
  }
}

module.exports = { supersededSnapshots, retainCompilerCaches };
