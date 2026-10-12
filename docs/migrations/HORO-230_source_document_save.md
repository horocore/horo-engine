# HORO-230 / #230 / [EDT-003.5] source persistence

Source persistence now belongs to SourceDocumentService rather than widget or navigator
filesystem callbacks. Existing Open/Edit/InspectDisk/Reload callers remain compatible.
Hosts use SourceSaveRequest with the exact current revision, inspect the typed receipt,
and retain dirty state for VisibleDurabilityUnconfirmed. Explicit overwrite consent is
an owned copy of the exact external bytes; it is never a blanket overwrite flag.

Workspace integrations dispatch the new source persistence commands and consume
SourceSaveOutcome, SourceSaveAllOutcome and SourceCloseOutcome. Save All includes each
dirty document and each individual failure. Close uses Cancel, Save or Discard; a failed
or unconfirmed Save retains the document. Save As is routed through SourceFileOpenService
so source snapshots and the workspace registry commit one location/instance identity.

DurableFileSystem implementations used for source save must implement WritePrivateDurable
and AtomicReplaceTracked. The former exclusively creates an absolute sibling temporary
in an existing host-owned parent, records creation before subsequent failure, applies
requested destination permissions before flushing, and accepts empty bytes. The default
fails closed, preserving other existing filesystem adapters. NativeDurableFileSystem
implements it with native exclusive creation and no-follow/reparse rejection. General
WriteDurable and AppendPrivateDurable contracts remain unchanged.

No public header ownership changes: SourceDocumentService, SourceFileOpenService and
EditorSurfaceIdentity remain owned by HoroEditorServices, Platform by its existing
foundation/platform boundary. The affected staged-header consumer qualifies the new
save/SaveAs/batch/close API without an implementation include path.
