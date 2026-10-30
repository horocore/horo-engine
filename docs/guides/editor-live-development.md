# Local editor UI iteration

The Build Job menu entry opens a separate **Build** request/activity modal based
on Mock Studio's `BuildDesign.tsx` and `WorkflowActivity.tsx`. Its form selects
product, workflow and target; starting a supported native compilation shows real
service phases and correlated build output. The current host supports Compile
Only for Game Runtime gameplay modules on the host platform/architecture.
Playable packaging, cooking, saved profiles, post-build tests and launching are
not connected; unsupported requests cannot start. Closing or keeping a job in
the background never cancels it. Reopening reconnects to the host-retained job.

Activity keeps the stages, summary and footer fixed; only the log container
scrolls. Active stages and busy indicators animate using editor motion tokens
and stop for terminal jobs. A host-owned provider polls the retained build every
250 ms. While its modal is closed, the status bar shows the current phase and
only service-provided progress; clicking it reopens the existing job. Completion,
failure and cancellation remove the busy contribution. The modal does not own
this provider or cancel work when it closes.

Prepare Release follows Mock Studio's `ReleaseDesign.tsx`: a sectioned request
form, a separate review table, and service-backed activity. Identity, target and
content, quality gates, security and local output stay in a transient modal
form. Review never submits an execution plan. The preparation-stage strip reads
service observations; there is no simulated job
progress, package inventory, signing identity or publication destination.
Release preparation remains unavailable until host preflight and execution
adapters are composed. Editing form preferences never executes scripts, creates
artifacts or publishes a candidate. Build and Release have distinct menu surfaces
and live drawing registrations.

Draft and review views omit the activity summary. Review groups are paginated
below the table; only the table supplies its border. Modal wheel events are
delivered directly to ImGui and clear queued workspace scrolling, so form input
does not inherit the workspace's smoothing tail.

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

On native macOS with Xcode Command Line Tools (AppleClang), on Apple Silicon or
Intel:

```bash
python3 scripts/dev.py run editor --live-ui --theme-file assets/themes/editor-development.json
```

Both paths require Debug. Cross-compilation is unsupported. The macOS compiler
uses the configured host compiler, SDK, architecture, deployment target and
compile definitions. The theme option is independent and can be omitted.

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

### Implementation ownership

This facility edits Horo Engine's built-in editor GUI during engine development.
It does not compile game/project sources, gameplay modules or external plugins.

All first-party implementation is below `src/editor/development/live_reload/`:

- `EditorGuiLiveReload.h/.cpp`: the common owner-thread service for startup,
  runtime lifetime, diagnostics, typed lookup and frame-boundary publication.
- `GuiLiveBuildRevision.h`: host-build identity used to invalidate cached objects.
- `rccpp/GuiRuntimeBridge.h`: private RCC++ registration and module table bridge.
- `rccpp/platform/macos/`: native AppleClang compiler transport and kqueue watcher.

Windows uses the pinned RCC++ `RuntimeCompiler/Compiler_PlatformWindows.cpp` and
`RuntimeCompiler/SimpleFileWatcher/FileWatcherWin32.cpp`, with the reviewed
`cmake/patches/rccpp/horo-msvc-transport.patch`. There is no second Windows-specific
editor service. Both platforms run the same `HoroEditorGuiLiveReloadHost` service.

`cmake/HoroRccppDevelopment.cmake` owns the dependency and native source selection.
`cmake/HoroEditorLiveUi.cmake` owns built-in drawing registrations and adapters.
`cmake/HoroEditorGuiLiveReloadHost.cmake` owns the internal service target and
per-executable generated `EditorGuiLiveReloadBuildConfig.h`. Executable composition
passes that typed configuration to `Start`; it does not select native compiler
behavior in C++. Adding a platform requires a reviewed native dependency/transport
selection and host compile/import configuration, plus native qualification; it does
not require duplicating the service. Unqualified platforms remain rejected.

The service commits its runtime only after successful startup. Missing host images
or include manifests return typed errors, permit a corrected retry, and publish no
partial runtime. A repeated successful `Start` keeps the current runtime intact.

### Ownership and compatibility

The original host owns panel/modal instances, search buffers, selections,
documents, projects and renderer resources. Replacement functions borrow those
objects synchronously. Keep mutable presentation state in those objects or
explicit state arguments, not runtime-module globals or function-local statics.
Do not retain a typed binding after its current call/frame. Adding/removing
fields, changing headers/signatures, adding registrations or changing build
options requires a host rebuild and restart. Changes confined to registered
`.cpp` bodies can reload while it runs.

A development-only Windows import library or macOS bundle loader routes existing
Horo/ImGui function calls back to the running executable. Modules do not link another copy of
ImGui, the theme authority or engine services. Imports are generated from the
exact HoroGui COFF objects on Windows; macOS exports host definitions and links
bundles against the running host image. macOS live-UI hosts retain the complete
GUI archive, so registered drawing dependencies are available even in a small
qualification host that does not normally construct every panel. Both use the host compile definitions
and include paths. If a new
call needs a host symbol absent from that build's imports, rebuild the host;
a failed link safely keeps the last working code. Modules use the same native
compiler and header ABI as the host, including the Debug CRT on Windows. This is trusted built-in GUI
code, not a plugin/extension interface; external packages retain their schema
boundary.

For shared immutable descriptors or inline factories that reference host globals,
use `GuiLive::HostSymbol<&Namespace::Symbol>()`. This imports a typed address
getter from the host instead of duplicating data in the replacement module.
For a function, invoke the returned reference with its normal arguments.

Intermediate objects stay below the build directory, replacement modules use
the OS temporary directory, and the runtime owns their lifetime until host GUI
users have stopped. The private Windows compiler patch owns child processes
and joins its output reader during teardown. The macOS adapter owns a compiler
process group, terminates/reaps it during teardown, streams its command script through
an owned temporary input file (avoiding macOS argument-size limits), and collects diagnostics in
a temporary file so verbose output cannot fill a pipe and stall compilation.
Its nonblocking kqueue watcher observes file writes and reconciles directory
events to reattach after atomic replacement. File events compare modification time
and size; compiler reads and access-time-only metadata changes do not trigger
another rebuild. Compilation/link failure keeps
the installed module; malformed candidates are never published.
The local macOS protection patch leaves debugger-owned Mach exception ports
unchanged; RCC++ retains its ordinary POSIX protected-call path.

`--live-ui` selects `HORO_EDITOR_LIVE_UI=ON` for native Windows x64/MSVC or
macOS/AppleClang Debug.
Normal `dev.py run editor` explicitly turns it off. Release builds and other
platforms retain ordinary static GUI behavior.

`HoroRccppMacTransportTests` covers native bundle imports, host address identity,
verbose compile failure/recovery, process teardown and save/replace/delete
watching. `HoroEditorGuiLiveReloadTests` exercises the full registered GUI with
headless drawing, state retention and shared-control updates. A transport test
pass alone does not qualify the full editor integration or manual UI interaction.

References:

- [RCC++ Dear ImGui example](https://github.com/enkisoftware/RCCpp-DearImGui-GLFW-example)
- [RuntimeCompiledCPlusPlus](https://github.com/RuntimeCompiledCPlusPlus/RuntimeCompiledCPlusPlus)
