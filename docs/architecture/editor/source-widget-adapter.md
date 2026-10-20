# Embedded source widget adapter

`SourceDocumentService` remains the canonical owner of exact source bytes,
revisions, persistence and document history. The workspace owns view lifetime and
issues nonzero view identities monotonically; closing and reopening a view never
reuses an identity during that project session.

`src/editor/source_editor/SourceEditorWidget.h` is the replaceable, GUI-private
boundary. `Publish` accepts an immutable authoritative source lease. `Draw`
returns an owning, revision-fenced replacement or history intent; the controller
applies it through the source owner and publishes the resulting snapshot.
`Selection`, `View`, `Identity`, `Select`, `Scroll`, `Decorate` and `Restore`
handle presentation only. `Draw` emits the canonical `SourceEditingCommand`
from the parent editing delivery; no parallel controller mutation path exists.
`SourceEditorTools` binds document history, find/replace and line navigation
through the same semantic command/planning API and existing localized controls.
No third-party types cross this boundary, and source document contracts require
no migration when replacing the widget implementation.

Each view has its own Zep editor, display, selection and scrolling state. Zep's
filesystem adapter rejects all reads, writes, directory scans and configuration
lookups. Its thread pool runs synchronously with `DisableThreads`. Native editing
commands are transient; Zep undo and redo stacks are cleared each frame. History
shortcuts leave the widget as typed document intents. A history result restores
only the matching originating view ID and resulting document revision. Other
views retain their selection and scroll state, clamping selection to valid new
UTF-8 boundaries when necessary.

Zep is pinned to `7aa7ed940d5c889dc6b27a2529a740c0bb2ad6af`. The dependency is
GUI-only; its upstream top-level CMake is not executed, avoiding ambient test,
toolchain, installation and compiler-setting changes. The source list is explicit
and its reviewed license is installed with the application notices. Zep remains a
private GUI implementation dependency. The adapter's only production caller is
the workspace source-document presentation in `EditorWorkspaceViewSequence.cpp`.

Zep requires LF text. The adapter keeps an LF presentation mirror and omits a
leading BOM from that mirror, while all external ranges and edits use original
document byte offsets. Changes compare scalar-aligned common prefixes/suffixes;
only the changed source range is replaced. Untouched BOM, CRLF, CR and mixed line
endings survive byte-for-byte. Newly inserted lines use the first authored newline
family, defaulting to LF for a document without newlines. A rejected edit or an
external replacement reconciles from the authoritative lease on the next frame.

The adapter uses the host font, frame scale, semantic GUI palette and clipboard.
Committed IME characters bypass Zep's eight-bit key converter and enter as UTF-8.
`IMGUI_USE_WCHAR32` is shared by the ImGui target and all of its consumers; this is
a deliberate private GUI ABI evolution and requires rebuilding those consumers.
`SourceWidgetPlatform.h/.cpp` contains the target-private Zep display,
filesystem, selection-mode and frame-theme bridge. The rendered insertion-caret
rectangle anchors native IME composition. Input
ownership follows the focused child surface; disabled scopes and popups suspend
editing, and changing tabs hands focus to the newly selected source view.

Decorations are externally supplied, revision-fenced document byte ranges. The
adapter does not own language selection, parsing, semantic intelligence or theme
resources. Syntax/theme delivery can replace the private presentation bridge
without changing document or history contracts.

## Qualification

`HoroSourceEditorWidgetTests` drives the actual adapter and document service with
headless ImGui frames for independent views, Unicode/IME commits, clipboard,
focus, disabled input, scale changes, rejection reconciliation, decorations and
teardown. Platform IME composition windows, native clipboard integrations and
interactive mouse/keyboard behavior additionally require a desktop smoke run.
Headless test results do not claim those native checks passed.
