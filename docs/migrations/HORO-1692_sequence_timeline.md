# Sequence timeline foundation

HORO-1692 / #1733 / [CIN-005.2] introduces `.hsequence` as an explicit default
source-opener policy for the existing versioned `SequenceAsset` JSON format.
There was no previously defined sequence suffix to migrate. Projects may override
the policy's `sequenceExtensions` without changing asset identity or schema.

`DocumentKind::Sequence` is appended to the public identity enum and persists as
`sequence`. `SequenceDocument.h` belongs to `HoroEditorServices`; the shared
timeline control belongs to `HoroGui` and consumes that declared dependency.
The public-header consumer includes the document contract.
`SequenceDocument::Open` borrows its identity by const reference for admission,
then copies it into the returned owned document; existing callers need no changes.

Asset activation admits the canonical project-contained source through
`SourceFileOpenService`, validates its bounded schema, and opens a persistent
`WorkspacePanelHost` document tab. The tab host is the single focus authority; `ClearDocumentFocus()` transfers
presentation back to native workspace panels without closing documents. The
workspace owns an immutable source lease
and exact SHA-256 revision. Equivalent paths focus the same session; equal asset
IDs reuse it only for the same revision. Changed bytes require explicitly closing
the retained session before reopening. Invalid sources do not leave ghost tabs.

Zoom, first visible frame and playhead are transient workspace presentation.
They do not write source files, enter scene history, mark a scene dirty or launch
runtime playback. This foundation has no pinned scene authoring context and
states that explicitly. Binding and key authoring remain in their owning tickets.

Transform, property, camera-cut and event track lanes are available. Audio and
sub-sequence metadata is preserved and reported as unavailable. The currently
persisted model contains key counts but no key times: the view reports counts
and never fabricates key positions. Later payload/editor tickets can extend the
shared component with real curves and key handles.

Navigation uses native styled controls; lanes use active theme tokens. Labels
wrap without reducing typography, document tabs scroll horizontally and track
rows scroll vertically. Focus, close and presentation commands carry typed
document instances so stale commands cannot operate on a reopened session.
