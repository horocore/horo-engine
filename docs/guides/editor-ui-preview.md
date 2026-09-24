# Editor UI Preview

The editor can open a representative workflow directly from the command line.
The native gallery uses the real theme, fonts, controls, and modal host
without opening a project or entering the Welcome route. Its own canvas and
scenario rail replace the normal editor screen and status bar while preview
mode is active.

Build the editor, then list and open scenarios:

```sh
cmake --build build/dev-editor --target HoroEditor
build/dev-editor/apps/HoroEditor.app/Contents/MacOS/HoroEditor --ui-preview=list
build/dev-editor/apps/HoroEditor.app/Contents/MacOS/HoroEditor --renderer opengl --ui-preview=asset-import
build/dev-editor/apps/HoroEditor.app/Contents/MacOS/HoroEditor --renderer opengl --ui-preview=asset-import-advanced
build/dev-editor/apps/HoroEditor.app/Contents/MacOS/HoroEditor --renderer opengl --ui-preview=asset-import-unsupported
build/dev-editor/apps/HoroEditor.app/Contents/MacOS/HoroEditor --renderer opengl --ui-preview=asset-import-empty
build/dev-editor/apps/HoroEditor.app/Contents/MacOS/HoroEditor --renderer opengl --ui-preview=viewport
```

The Asset Import scenarios cover a populated batch with four representative
files and one warning, the same batch with Advanced expanded, an unsupported
file with its diagnostic, and the empty state. Their data is held only in memory.
Model cards use the standard asset-type icon because fixture paths are in memory
and do not point to source files. In a real import, the selected file is prepared
by its registered importer on a background job and the provider's image is shown
before the asset is committed. The icon remains the fallback when no preview is
available.
Browsing, destination changes, and importing are
inert in preview mode, so it does not write to a project. File selection and
control-state changes remain available for inspecting visual states. Close the
modal to see the unobstructed canvas, then use the scenario rail to reopen it.

To add a scenario, register its ID and description in
`src/editor/ui_preview/EditorUiPreviewCatalog.h` and provide the scenario data
and modal adapter under `src/editor/ui_preview/`. That folder owns the gallery,
scenario composition, and in-memory fixtures. The preview adapter imports the
production modal and renders it through the regular modal host; the production
modal contains no scenario names or sample files. Its read-only presentation
settings keep file dialogs and imports inactive in the gallery.

The `viewport` scenario draws an illustrative scene in memory and uses the
production viewport overlay component. Its projection, tool, and grid controls
are interactive. Geometry statistics are sample data in this scenario; the live
viewport only shows counts available from the workspace model.
