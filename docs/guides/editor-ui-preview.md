# Editor UI Preview

The editor can open a representative workflow directly from the command line.
The native gallery uses the real theme, fonts, controls, renderer, and modal host
without opening a project or entering the Welcome route. Its own canvas and
scenario rail replace the normal editor screen and status bar while preview
mode is active.

Build the editor, then list and open scenarios:

```sh
cmake --build build/dev-editor --target HoroEditor
build/dev-editor/apps/HoroEditor.app/Contents/MacOS/HoroEditor --ui-preview=list
build/dev-editor/apps/HoroEditor.app/Contents/MacOS/HoroEditor --renderer opengl --ui-preview=asset-import
build/dev-editor/apps/HoroEditor.app/Contents/MacOS/HoroEditor --renderer opengl --ui-preview=asset-import-empty
```

The Asset Import scenarios cover a populated batch with four representative
files and one warning, plus the empty state. Their data is held only in memory.
Browsing, destination changes, and importing are
inert in preview mode, so it does not write to a project. File selection and
control-state changes remain available for inspecting visual states. Close the
modal to see the unobstructed canvas, then use the scenario rail to reopen it.

To add a scenario, register its ID and description in
`src/editor/ui_preview/EditorUiPreviewCatalog.h`, open its surface from
`GuiScreenHost::OpenUiPreview`, and provide representative in-memory state
from the owning feature. `EditorUiPreviewGallery` owns only gallery chrome;
the regular modal host and feature presentation draw the scenario itself. Keep
preview data and mutating actions separate from normal workflows.
