# Prefab Component Override Delivery

HORO-1029 / [PFB-003.3] / GitHub #1029 supplies the component topology prerequisite
of M3's #1031. Its own milestone does not change that native dependency.

The existing value override envelope also owns typed component topology records.
`PreparePrefabComponentEdit` and `EditScenePrefabComponentCommand` prepare these
records through the shared resolver and canonical document history.

## Contract

- Extend the existing instance override envelope, resolver and document history;
  do not add another scene-owned override store or mutate source prefab files.
- Persist only ADR-093 `AddComponent` and `RemoveComponent` operations. Replacement
  is one prepared transaction removing the old occurrence and adding a fresh
  component identity. Reverting selected records is another document transaction.
- Address the exact nested object, component type and nonzero occurrence ID.
  A remove carries the expected canonical source-component digest and schema.
- Capture inert, bounded component admission metadata with the existing schema
  snapshot: payload schema, multiplicity, explicit topology permissions and typed
  dependencies/compatibility. Never infer dependencies from names or run providers.
- Validate the complete detached candidate before publication. Duplicate IDs,
  same-layer property patches of added/removed components, unsatisfied dependencies,
  incompatible behaviors, cancellation and budget exhaustion publish nothing.
- Preserve source changes, unavailable schemas and deleted targets as owned
  conflicts/orphans. Resolution, serialization and reload must never retarget or
  discard retained intent silently.

## Format And Caller Migration

Keep existing value-only canonical V1 bytes unchanged. Component-bearing intent
uses ADR-093's existing V1 operation ranks 5/6 in the same records, conflicts and
orphans arrays. Readers implementing only AssignValue reject unsupported operation
ranks explicitly; there is no implicit project rewrite. Runtime cook rejects
unresolved component intent through the same effective-candidate admission as
value overrides.

The authoring schema capture, override model, SceneSource codec/projection,
document transaction/history and cook callers must be updated together. Account
for component records and preserved payloads in the existing count/byte and history
budgets. New public headers belong to `HoroPrefabOverrideModel`; validate every
affected public-header consumer and the declared dependency direction.

## Required Evidence

Exercise add/remove/replacement and revert through document undo/redo; exact source
and schema changes; provider unload/reload; duplicate identity, missing dependencies
and behavior incompatibility; canonical round trips; source-order independence;
cancellation, malformed inputs and resource limits. Validate unchanged source
snapshots and no partial document/runtime publication on failure. Run the actual
PR-scope build, coverage, secrets and Codacy checks and report managed local Sonar
availability separately from hosted gates.
