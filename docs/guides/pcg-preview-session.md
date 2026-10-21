# Cancellable PCG preview

`PCGPreviewSession` is an additive EditorServices contract. It exists because the
canonical PCG document owns source/history/save state and must not acquire worker,
renderer or target-commit ownership. No existing public signature changes. The
registered graph canvas is its production caller; the public-header consumer
protects the new boundary. No native renderer types enter the public API.

The workspace composes the ordinary executable catalog, a process scheduler and
explicit preview inputs. The default disposable sandbox owns grid 1, box 2 and
surface 3, with distinct provider/element identities and canonical content evidence.
It is labeled as a sandbox, never production Scene truth. A host may replace the
immutable snapshot/schema/finite evaluation limits through
`ReplacePCGPreviewInputs`; replacement revokes every older overlay immediately.
Graphs requiring other provider elements fail with the actual cooker/evaluator
cause. The preview never silently substitutes missing elements.

Press Preview to capture unsaved source, exact session/document/catalog revisions,
provider root/revision/origin and finite limits. A background CPU job builds an
operation-private registry from the retained executable catalog, validates/cooks
the exact source through the ordinary cooker and invokes `EvaluatePCGCpu`. There
is no second interpreter, runtime async coordinator, time slicing, scene-output
adapter, authoritative feature mutation or commit transaction. Capability grants
allow cooking the authored execution mode; this object owns no capability that
can publish output into a production target.

Each document has at most one running job and one coalesced pending capture.
Replacement cancels old intent, releases its current overlay and waits for the
old worker to terminate before admitting the latest capture. Workers retain only
immutable owned inputs and one mutex-protected mailbox. The mailbox lock protects
the candidate handoff; no evaluation, callbacks or waits occur under it. Frame
advancement polls durable job records without waiting or pumping. Up to eight
document sessions may own previews in a workspace; individual tier budgets and
point/node visit ceilings apply, and each preview uses one CPU job with no nested
workers or borrowed scheduler in the evaluator.

Every publication and each subsequent frame revalidate the current document and
provider fences. Edit, undo/redo, reload, catalog/provider replacement, explicit
clear, parent cancellation, canonical close and shutdown revoke the overlay.
An older/cancelled completion cannot become current, even if it completed before
cancellation. A new request supersedes an identical older request. Panel hiding,
refocusing and detachment preserve the document-owned preview workflow.

The view draws canonical final-output point positions and per-point generated
output bounds in a clipped top-down X/Z overlay. Work is capped globally at 8192
points per frame across all outputs. It uses shared controls, typography, theme
colors and English/Turkish copy; narrow widths wrap controls and status. Candidates
remain immutable and the draw path performs no source, Scene or feature mutation.
Snapshots are read-only leases. Outstanding old candidates are charged at
replacement admission, and at most 16 retained overlay generations are allowed.
Consumers should release obsolete leases. They cannot publish a leased result.

Explicit `Shutdown` closes admission and cancels/joins to a finite deadline,
retaining ownership on timeout so the host can retry. Canonical workspace close
and object destruction use a cooperative lifecycle join as a fallback and never
detach accepted workers. The process scheduler must outlive the workspace/session.

Regression coverage lives in `HoroPCGDocumentTests`,
`HoroEditorWorkspaceControllerTests`, `HoroPCGDocumentPublicHeaderConsumer` and
`HoroEditorLocalizationServiceTests`. It covers real cook/evaluation, document
isolation, coalescing, cancellation, stale/reopened source/provider fences, failure,
queued shutdown, owner-thread admission and actual bounded ImGui drawing at narrow
and wide sizes in both themes/locales. Compilation, execution and interactive
visual qualification must be reported separately from source-only checks.

## Authoring stack integration

The merge order is schema/property authoring (PR #3510), then preview (PR #3509).
`PCGEditorCatalog.schemas` is the only executable catalog publication consumed by
source support, inspector drafts and preview capture. Default catalog composition
grants validation only; the interactive preview host passes its execution grants
explicitly. Validation-only hosts can author source but cannot admit evaluation.

Preview always captures the applied immutable document root. An unapplied inspector
draft never enters a request. Apply, undo and redo all use the canonical document
executor and revoke the old overlay; refresh evaluates the resulting revision. A
cancelled Apply preserves the current root/overlay, and preview Cancel clears only
the disposable overlay. Integration regression cases protect those boundaries.
