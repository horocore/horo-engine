# HORO-2027 offline Scene bake

The transaction boundary is required because detached PCG generation plans cannot
publish authored Scene objects, ownership and the editor history atomically.
`HoroEngine::PCGSceneBake` now composes those owners through public contracts; neither
PCG core nor graph document code imports EditorModel private headers.

## Callers and migration

Existing SceneSource consumers must rebuild: `SceneObjectSnapshot` gains an optional
`generated` ownership annotation. Existing aggregate initializers retain their
meaning because the field is appended and defaults empty. Existing authored Scene
schema-1 sources remain readable and their encoded bytes remain unchanged when the
annotation is absent. New sources persist the optional typed annotation. Consumers
that project Scene objects must retain it when preserving generated ownership, or
explicitly clear it when adopting authored content. Payload fingerprints exclude
this annotation to avoid self-referential hashes; ownership is compared separately.

Headless callers link `HoroEngine::PCGSceneBake`, create a canonical
`SceneDocumentTransactions`, and capture a `PCGSceneBakeTarget`. Editor composition
may bind its existing canonical SceneDocument and sole EditorHistory instead; those
borrowed owners must outlive the port, and replacement requires a fresh binding.
The host supplies a non-reused document opening generation and explicit permission,
capability and cancellation evidence. No constructor registers ambient services.

Evaluation captures `PCGGraphDocument::Snapshot` and includes the exact
`CapturePCGSceneGraphInput` dependency in its immutable generation plan. Translation
uses the captured target receipt, ownership and occupied IDs. Bake receives that
working snapshot and fresh target/asset evidence. A failure preserves both document
states. Saving the graph is independent from committing or saving the Scene.

Use the existing Scene history through `Replay` for undo/redo. Historical values
include exact authored ordering and provenance, so replay needs no graph document,
asset provider or evaluator. Legacy commands retain their existing history path;
only aggregate replacement entries use detached validation and atomic replay.

## Validation scope

The regressions cover working unsaved graph bake, exact idempotence, update/remove,
undo/redo, dirty-state restoration, malformed late values, hierarchy validation,
permission/cancellation/budget/stale graph rejection, ownership adoption and
replacement/shutdown/thread boundaries. The real public consumer uses only staged
headers. Windows build and discovery inventories include the bake tests, consumer
and generated public-header consumers. Execution results are recorded separately
in the delivery PR; authored coverage alone is not a passing native test result.

The target receipt identity changes with authored revision, while its owner generation
is the stable document opening generation. An unrelated authored edit expires old
receipts but does not transfer generated ownership. Reopening/replacing the document
uses a new host-issued session generation and rejects work from the previous opening.
