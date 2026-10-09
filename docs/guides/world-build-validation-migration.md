# World Build Validation Integration

Hosts can add `HoroEngine::SceneCellPayload` validation without changing existing
Scene cook/cache or WorldStreaming runtime contracts. No existing API changes.

After complete offline cell cooking, capture `WorldBuildCell` identities for
all descriptor cells, immutable payload leases and required `WorldBuildReference`
endpoints from the authoritative authoring snapshot. Supply positive count,
source-byte, diagnostic and logical payload estimate ceilings. A null payload
lease represents missing output; empty cells still require a cooked baseline.

Call `ValidateWorldBuild` on the tooling operation thread. Treat a typed failure
as incomplete validation; a returned report with `CanPublish() == false` blocks
build publication. Before appending the report's `BuildOutputRecord` values to
the host-owned store, compare its revision to the current build generation and
discard superseded output. Attach host session/operation correlation when
appending. Relative or embedded-NUL navigation paths are rejected; omit a source
location when none is available. This seam does not validate provider/native or
encoded artifact sizes and does not replace their existing owner validations.

Cancel and synchronously finish validation before destroying input snapshots
or stores. No background task, service registration or runtime state is owned
by this function. Existing callers of `IncrementalSceneCellCook` remain valid;
this validation is an explicit build publication step, separate from cache
replacement and runtime attachment.
