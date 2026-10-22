# Navigation bake workflow

Open Navigation from the workspace activity bar. The persistent bottom panel is
closed in the default layout. It reads saved project policy, definition profiles
and exact Scene-qualified surfaces through a bounded background capture. Save
Scene and definition drafts before baking. Author profiles and build/tile policy
in the definition document, and capacity/provider policy in Project Settings;
Navigation does not maintain another editable profile catalog.

Bake and publish captures fresh canonical source through NavigationBakeApplication.
Validate uses the same capture and checks without publishing. Optional selected
surfaces preserve the complete replacement closure: every unselected tile must
have verified retained output. Disabled and empty authored surfaces remain selectable.

The executable owns the application and workers. Panel hide/close, document close
and cancellation-dialog dismissal keep accepted work running. The cancellation
modal offers keep running, cancel and return. Only explicit confirmation requests
cancellation of its exact opening identity. A stale confirmation cannot target a
later retry. Shutdown revokes the native source owner, cancels and drains work
before releasing the application, scheduler or process OperationStore.

The panel projects actual application state, shared bake OperationId, stage identifier,
progress (or indeterminate capture), elapsed seconds, captured surface/tile counts,
configured resource ceilings, typed failure codes and rebuilt/reused publication
counts. Ceilings are not live memory measurements. Shared Operations remains the
canonical process operation list; the panel does not write progress records.
Retry preserves execution and surface intent but starts a new identity and captures
fresh source. It never reuses the failed/cancelled capture or borrows modal state.

## Host and header migration

NavigationBakeWorkspace.h is a backend-neutral EditorServices public contract,
owned once in the header registry and covered by its isolated consumer target.
EditorServices now declares NavigationBakeAutomation as a public dependency because
its copied projections contain that application's typed request/result contracts.
No native provider or platform handle enters this surface.

The executable-only NativeNavigationBakeComposition contract is staged in a
non-installed internal header target. CLI, MCP and HoroEditor now call this single
composition authority for the same Recast builder, compatibility stamps, native
root owners, headless-null artifact target, immutable cache and AssetCook output
namespace. Existing CLI/MCP callers retain their prior invocation deadline and
cancellation fences. File-backed source verification and the existing exclusive
writer lease protect final publication across executable processes.

No provider fallback is introduced. Unsupported providers, incompatible projects,
unadmitted package source, invalid source geometry or missing prerequisites return
the original typed failure. This workflow does not enable package execution or
claim support beyond canonical source capture. Catalog choices are an opening
snapshot; Reload saved project choices refreshes them after accepted work drains.
A monotonically new opening identity rejects old modal/retry control keys even when
the new application restarts its command counter. Every start/retry recaptures source.

## Qualification

HoroEditorNavigationBakeHostTests exercises genuine native project source capture,
Scene-qualified selection, detached views and dismissed confirmation, shared operation
projection, concurrent-start rejection, exact-id cancellation, fresh-source retries,
publication/cache reuse and shutdown drain. Existing bake-service tests retain the
exclusive-publication and stale-source transaction coverage. The public consumer
checks the exported boundary, and localization regression checks paired en-US/tr-TR
copy. Native builds, tests and UI interaction need explicit validation evidence;
source inspection alone does not qualify them.
