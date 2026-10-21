# Navigation component inspectors

HORO-1292 / NAV-007.2 uses the existing Scene document and semantic history.
Surface, region, modifier, link and agent cards use shared editor controls and
localized EN/TR labels. Profile sets retain absent identities until explicitly
repaired. Reference selectors preserve exact IDs and never select the first
available descriptor as a fallback. Link endpoints remain ordered object-local
coordinates; changing direction does not swap them.

Select several authored objects to stage their component values independently.
Apply submits all changed occurrences in one canonical Scene transaction. The
transaction validates the entire proposed navigation graph before publication;
undo and redo restore the complete gesture. Untouched values, unrelated
components, and stable object/component identities remain unchanged. Shape
changes update their closed typed payload. Invalid dimensions, missing descriptors,
coincident endpoints and incompatible profiles remain uncommitted drafts. Cancel
restores the captured values. Selection, document opening/revision and descriptor
publication changes revoke old drafts. Runtime conversion sees committed values.

A selected Prefab member uses the same cards. The workspace owns source capture,
the immutable schema and the canonical instance command. Apply captures current
source bytes under the explicit host mutation lease, checks the original source,
schema, opening, document and selection publication, and prepares all component
replacements before recording one override-history entry. Replacement uses fresh
occurrence IDs and retains exact source preconditions; it does not modify source
assets or mutate a placement in the panel. Canonical full Scene conversion and
exact descriptor checks run before publication. Missing project/custom metadata,
broken/stale sources and unresolved overrides remain visible and block activation.
The current hierarchy selection contract identifies one exact Prefab member;
ordinary Scene multi-selection stages several objects together.

## Additive contract and ownership migration

`HoroEditorModel` owns the new public `NavigationInspector.h`. It prepares typed
navigation replacements for the existing `SceneDocumentTransactions` port; GUI
and host code own neither a second document nor a second history. Existing
single-object navigation commands remain valid. `HoroSceneRuntimeConversion`
adds `EncodePrefabNavigationComponent` beside its existing built-in projector,
using the canonical Scene component writer/reader rather than UI-authored JSON.
The ownership registry and both affected public-header consumer sources change
with these APIs. No backend-native handles or broad include roots are added.

The workspace explicitly composes inert built-in component topology metadata.
Only navigation replacements receive add/remove permission; other known built-ins
retain multiplicity metadata and no new topology permission. Unavailable custom
component or behavior schemas do not acquire implicit permission. Preview, cook
and project-wide native schema composition remain their existing owners.

## Validation scope

Regression sources cover all five typed families, atomic multi-object history,
no-op/cancel/stale/resource rejection, missing references, invalid shape and
endpoint drafts, source/schema/opening fences for Prefab batch commands, draft
revocation, public consumers, EN/TR messages, and narrow shared-control rendering.
Headless ImGui rendering does not establish a native device/manual interaction
pass. Run the affected model, inspector session/render, localization and public
consumer targets in CI or an explicitly granted local validation window.
