# Local editor UI iteration

The Debug editor can watch the selected custom theme without a C++ rebuild:

```powershell
python .\scripts\dev.py run editor --theme-file .\assets\themes\editor-development.json
```

Edit the JSON while the editor is open. One background I/O inspection at most
every 500 ms reads a bounded 256 KiB candidate. The GUI owner installs a valid
replacement before the next frame and publishes `Theme::ThemeReloadedEvent`.
Malformed, missing, oversized or partially written files keep the last good
theme. Switching to another preset fences outstanding candidates from the old
source. Only the selected file is watched; development mode does not rewrite
the saved settings or scan directories every frame.

Supported overrides include `colors`, `tokens.radii`, `tokens.styleSpacing`,
`tokens.componentSizes`, `tokens.layoutSpacing`, `tokens.layoutSizes`,
`tokens.motion`, `tokens.sourceSyntax` and logical typography roles. Existing
surfaces must read live tokens to react; constants embedded in C++ still need a
code build. Font atlas creation remains a startup operation: this development
watcher does not replace atlas resources or the installed font files.

The selected file is passed through `HORO_THEME_OVERRIDE`; this explicit startup
selection takes precedence over the persisted preset. Normal Settings theme
selection can subsequently switch away from it. Release builds do not poll
theme files.

## Runtime-compiled GUI drawing

On native Windows x64 with MSVC installed:

```powershell
python .\scripts\dev.py run editor --live-ui --theme-file .\assets\themes\editor-development.json
```

The first build fetches RCC++ at pinned revision
`005c05145d98b87d974c36fc885003ea88bf3932` under its zlib license. There is no
paid SDK or separate demo window. Save a registered GUI `.cpp` file to compile
its actual drawing code. The GUI owner installs completed modules before the
next frame. Compile/link failures retain the previous drawing. Diagnostics use
`editor.live_ui`.

The shared interface is `GuiLive::IRegistry`, borrowed through
`ThemeContext::liveReload`. Typed calls preserve the exact argument types,
references, void/value return and function-signature fingerprint. There is no
welcome-specific virtual method. Ordinary disabled builds pass a null registry
and run their normal compiled drawing, without RCC++ linkage.

Registrations in `cmake/HoroGuiLiveSources.cmake` cover the shell and status bar,
welcome, project creation/loading, workspace, all built-in panels, dock panes,
snackbars and modal workflows. Common components (buttons, fields, tabs, popups,
icons, timeline controls and custom extension controls) and their presentation
helpers compile into the same replacement module as those entries. Their source
inventory comes from the owning `HoroGui` target's presentation families, rather
than requiring a registration in each component file. A component change updates
all registered drawing entries together; unchanged intermediate objects are reused.

`SUPPORT` can add helpers outside those families. New independently dispatched
panels/modals still need one entry registration. Screen controllers, theme/font ownership,
the native Zep source-widget backend and platform/render backends remain host code;
changing them requires rebuilding and restarting the host. Source-editor toolbar
controls participate in component reload. Inspect the CMake inventory for the
authoritative scope.

### Add another drawing entry

A free function needs one build registration:

```cmake
horo_register_live_gui(MyView editor/screens/my_view/MyView.cpp Horo::Editor::DrawMyView)
```

The screen host invokes the original function through the shared dispatcher:

```cpp
GuiLive::Invoke<&DrawMyView>(ctx.theme.liveReload, state, ctx);
```

The drawing function needs no guard, route name or self-dispatch. Registrations
derive their identity and signature from the original entry. Concrete members
use `MEMBER`, and their common host calls `Invoke<&MyPane::Draw>(registry, pane,
arguments...)`.

Workspace panels and modals use the existing shared hosts, so a new implementation
only needs its build registration:

```cmake
horo_register_live_gui(MyPanel editor/panels/MyPanel.cpp Horo::Editor::MyPanel::DrawPanel
    MEMBER BASE Horo::Editor::IWorkspacePanel)
horo_register_live_gui(MyModal editor/modals/MyModal.cpp Horo::Editor::MyModal::Draw
    MEMBER BASE Horo::Editor::EditorModal)
```

Their hosts call `InvokeVirtual` through the base interface. Generated adapters
match the original object's concrete type using a compiler-local type-name
fingerprint, check the exact base signature and explicitly call its replacement
implementation. This bypasses the old virtual table without changing it. Unknown
types, signature mismatches and null registries retain normal virtual drawing.
Nested calls from replaced workspace/dock code still dispatch to replaced child
panels. Modal creation, updates, input ownership and close decisions keep their
existing lifecycle; no registry is stored in modal instances.

`GuiLiveReload.h` is a backend-neutral `EditorServices` contract so the modal host
can own dispatch without depending on `HoroGui`. Registered drawing sources and
generated compiler adapters remain private to `HoroGui`. The application sees
only the narrow internal compiler bridge and the registry.

### Ownership and compatibility

The original host owns panel/modal instances, search buffers, selections,
documents, projects and renderer resources. Replacement functions borrow those
objects synchronously. Keep mutable presentation state in those objects or
explicit state arguments, not runtime-module globals or function-local statics.
Do not retain a typed binding after its current call/frame. Adding/removing
fields, changing headers/signatures, adding registrations or changing build
options requires a host rebuild and restart. Changes confined to registered
`.cpp` bodies can reload while it runs.

A development-only Windows import library routes existing Horo/ImGui function
calls back to the running executable. Modules do not link another copy of
ImGui, the theme authority or engine services. Imports are generated from the
exact HoroGui COFF objects and its compile definitions/include paths. If a new
call needs a host symbol absent from that build's imports, rebuild the host;
a failed link safely keeps the last working code. Modules use the same native
compiler, Debug CRT and header ABI as the host. This is trusted built-in GUI
code, not a plugin/extension interface; external packages retain their schema
boundary.

For shared immutable descriptors or inline factories that reference host globals,
use `GuiLive::HostSymbol<&Namespace::Symbol>()`. This imports a typed address
getter from the host instead of duplicating data in the replacement module.
For a function, invoke the returned reference with its normal arguments.

Intermediate objects stay below the build directory, replacement modules use
the OS temporary directory, and the runtime owns their lifetime until host GUI
users have stopped. The private Windows compiler patch owns child processes
and joins its output reader during teardown.

`--live-ui` selects `HORO_EDITOR_LIVE_UI=ON` for native Windows x64 MSVC Debug.
Normal `dev.py run editor` explicitly turns it off. Release builds and other
platforms retain ordinary static GUI behavior.

References:

- [RCC++ Dear ImGui example](https://github.com/enkisoftware/RCCpp-DearImGui-GLFW-example)
- [RuntimeCompiledCPlusPlus](https://github.com/RuntimeCompiledCPlusPlus/RuntimeCompiledCPlusPlus)
