# Render graph inspection

`RenderGraphInspection.h` is an additive RenderApi contract. Explicit tooling
capture copies one intact graph, schedule, lifetime plan and compiled execution
into a detached immutable value. It never resolves a resident handle or retains
a native resource, registry, frame scope or source graph. All source owners and
the deterministic schedule must match. Capture fails transactionally on invalid
identity, cancellation, allocation failure or capacity exhaustion.

The default allowance is 65,536 aggregate records and 16 MiB of owned record
storage. Hard bounds are 262,144 records and 16 MiB. Storage accounting includes
the snapshot object and copied record payloads; allocator/control-block overhead
is not an encoded-data allowance. Every count/byte charge is checked before
allocation. No data is silently truncated and the authored topology stays closed.

`RenderFrameScope::CaptureInspection` binds an explicit capture to the actual
frontend resource owner and backend frame token at the host's pre-execution safe
point. The snapshot describes a **planned logical graph**, even if that frame
later fails, changes its intended work or is aborted. It does not prove submission,
native barrier realization, GPU completion, timing, memory usage or performance.
Existing immediate-pass hosts do not fabricate a graph. The editor pane reports
no capture until the host explicitly requests one for a real compiled graph.

Capture/export may run over immutable detached compiler/snapshot inputs on a
worker with cooperative stop tokens. Feed publication/read/replacement/shutdown
and real-frame capture belong to their creating render owner thread. No callback,
mutex, native enumeration, GPU wait or automatic frame capture is introduced.
Cancellation suppresses unpublished results and never aborts rendering.

One `RenderGraphInspectionFeed` retains one immutable publication. Revisions must
increase within the exact renderer owner. Renderer replacement clears the slot
and rejects old-generation results; already returned owned values remain valid
historical evidence. Shutdown is idempotent and rejects new operations. Host
clients own their retention budgets. The optional editor pane retains one value,
formats at most 128 records per page only on source/section/page/language change,
and does no query or formatting work while hidden. Shared controls, minimum
typography and localization are used. Timing is explicitly unavailable.

## Export schema version 1

`ExportRenderGraphInspection` returns deterministic numeric-only UTF-8 JSON for
the already captured snapshot. Cancellation/capacity failure returns no partial
bytes. No shader/resource content, native handles, unrestricted names or paths
are exported. A caller that saves bytes must use its existing Platform path/trust
validation and atomic publication; this encoder performs no file I/O or recapture.

`schema` is `horo.render.graph.inspection`; `version` is `1`; `context` is
`[rendererOwner, backendFrameToken, captureRevision, graphOwner]`. IDs are exact
unsigned integers and scoped to this process/generation, not durable identities.
`coverage` is `complete_logical_whole_resource`; `timing` is `unavailable`.
Each numeric enum follows its current Horo declaration order (hazards retain
their bit set); a vocabulary change requires an export schema revision.

| Array | Tuple columns |
| --- | --- |
| passes | pass, kind, queue role, cull policy |
| dispositions | pass, retained/culled disposition, reason |
| execution | pass, kind, effective queue, in execution order |
| resources | resource, kind, lifetime class, binding kind (0 none / 1 buffer / 2 texture), resident owner, slot, generation |
| exports | observable resource |
| uses | pass, resource, access, usage kind, in authored order |
| dependencies | before pass, after pass, reason |
| lifetimes | resource, first pass, last pass, first execution index, last execution index, used/unused disposition |
| allocations | resource, compatibility class, logical allocation slot |
| aliases | prior resource, subsequent resource, compatibility class |
| transitions | resource, before pass, after pass, hazards, old state, new state |
| transfers | resource, release pass, acquire pass, source queue, destination queue |

Each transition state is `[access, operation, pipeline scope, layout intent,
effective queue]`. Invalid pass/queue zero means an imported/undefined boundary,
not a fabricated pass. Lifetimes marked unused do not present zero indices as
measured use. Allocation slots/alias opportunities grant no native aliasing.

This delivers the graph projection/export in RND-009.10. ADR-049's separate
RND-017.9 composite inspector remains responsible for memory/measurement joins,
retention/query services, incident manifests, atomic saved-artifact export and
advanced graph layout. The initial graph consumer does not claim those services
or native qualification. No existing callers or persisted graph formats need a
migration; snapshot ownership is assigned to RenderApi and consumer coverage
is updated with the new declarations. Shared allocation uses a private construction
key supplied only by validated capture; consumers cannot construct empty or forged
snapshots, including through an empty brace argument. The renderer-aware pane is private to the
HoroEditor application composition root. Gui retains no renderer dependency;
its existing dock chrome and the host pane share the public body-text primitive.
