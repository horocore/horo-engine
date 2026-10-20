# Viewport streaming preview integration

`WorldStreamingPreview` is an EditorModel-owned immutable projection. It joins the
existing controller document lease with one exact mounted registry, source plan and
runtime diagnostic publication. It owns bounded row copies and retains the source
lease; it does not register sources, select a renderer, change packaged policy or
infer residency from an open source page.

The composition root captures the input publications at a common safe point. It
passes the exact document, registry, plan, diagnostics and view revisions to
`Create`. Store a successful projection only while those publications are current;
clear the displayed projection immediately on replacement, project close,
cancellation or a failed refresh. Retained historical values remain readable but
must not be displayed against a newer view fence. Rebuild on publication changes
outside the draw path, with an explicit consumer retention budget.

The projection preserves desired state, pinned floor, priority/stability evidence,
source contributions (including predicted intent), actual generation-scoped cell
attempts, failure records and authority-authored decision events. Missing planner
rows remain absent demand. Missing runtime attempts remain absent evidence rather
than being synthesized as loaded/unloaded. Retiring generations remain separate.
Disabled decision instrumentation does not suppress typed failure causes.

## Spatial contract and public API migration

`WorldPartitionRegistrySnapshot::CellBounds(handle)` is an additive WorldStreaming
API. It resolves an exact registry handle and returns the index's existing inclusive
millimeter bounds. New viewport consumers use this accessor instead of repeating
grid multiplication, LOD scaling or overflow handling. Existing registry callers
need no migration. Its error semantics match `Resolve`; retained snapshots remain
readable after registry replacement and shutdown. The public-header ownership
remains WorldStreaming, and its Editor consumer remains EditorModel.

## Outstanding host prerequisite

HORO-1630 and HORO-1633 supply the canonical opened-world/view binding. This branch
currently supplies only the joined projection and exact bounds accessor. The
registered viewport overlay and the controller's actual planner/runtime diagnostic
producer are not yet connected. Until those production paths and their lifecycle
tests are integrated, HORO-1634 is incomplete and its PR must remain draft without
an issue-closing relationship. Fixture-created snapshots demonstrate the projection
contract only; they do not prove production runtime state is available.

No local configure, build or test has been authorized for this source freeze.
Hosted checks and a later manual viewport check are separate evidence requirements.
