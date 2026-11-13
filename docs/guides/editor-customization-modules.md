# Authoring editor customization modules

Use the normal Horo SDK/package workflow, trust policy and native module entry
points. An editor-presentation module can supply commands, themes and settings
pages. These interfaces extend the editor; they do not hot reload game source.
Editor GUI development hot reload remains a separate host development workflow.

## Commands and keymaps

Declare the owning module's `editor-presentation` role and required host
capabilities `editor.activity` and `editor.commands`. Declare the existing
`editor.activity_item` and `editor.panel` pair and one `editor.command` claim for
each command ID. Keep command IDs package-qualified, outside the reserved
`editor.` builtin namespace.

In `horo_extension_load`, publish a `HoroEditorActivityDescriptorV2` through
`host->registerEditorActivity`. Set `activity.structSize` to the extended record's
size and `activity.schemaVersion` to `2`. Keep the initial snapshot's own schema
version independent. The base descriptor still supplies the existing localized
messages, initial semantic form and `invokeAction` callback.

```c
const HoroEditorActivityCommand commands[] = {{
    .id = {"example.tools.run", sizeof("example.tools.run") - 1},
    .nodeId = {"example.run", sizeof("example.run") - 1},
    .actionId = {"example.run", sizeof("example.run") - 1},
    .labelKey = {"example.run.label", sizeof("example.run.label") - 1},
    .shortcut = {"Ctrl+Shift+P", sizeof("Ctrl+Shift+P") - 1}
}};
HoroEditorActivityDescriptorV2 contribution = {
    .activity = activity_descriptor,
    .commands = commands,
    .commandCount = 1
};
contribution.activity.structSize = sizeof(contribution);
contribution.activity.schemaVersion = 2;
HoroExtensionStatus result = host->registerEditorActivity(
    host->hostContext, &contribution.activity, &session);
```

The initial form must contain the referenced action node. Supply an `en-US`
message for the command label and optional additional locales. Up to 64 commands
are accepted per activity, within the shared registry's global capacity. Duplicate
IDs, shortcut collisions, missing claims, unknown action references and missing
capability negotiation reject registration and roll back activation. Strings are
copied before returning; keep all callbacks within the existing native lifetime.

Commands appear in Settings under **Code Editor → Keymaps**, alongside builtins.
The host records physical shortcuts, checks conflicts and persists overrides.
Use `Meta+Shift+P` for a macOS Command default if appropriate. The current GUI
adapter supports letters, digits, F1–F12, standard navigation keys, `=` and `-`.
An empty default leaves a command unbound until configured. Worker execution uses
the existing action callback and cooperative cancellation; never draw from it.

The executable C11 fixture `tests/fixtures/extensions/EditorActivityModule.c`,
built with `HORO_ACTIVITY_COMMAND_FIXTURE`, demonstrates the complete profile.

## Themes

Add a contribution to `extension.json`:

```json
{
  "type": "editor.theme",
  "id": "example.tools.midnight",
  "module": "example.tools.editor",
  "resource": "themes/midnight.json"
}
```

Ship the resource through the package's normal file manifest. Use the existing
theme JSON format (`name`, `colors`, `tokens`, including `sourceSyntax`). Theme
resources are bounded to 256 KiB. Paths must be relative JSON resources without
parent traversal or symlinks. The GUI host publishes themes only after successful
package activation. They appear in the existing Appearance theme selector and
are persisted by contribution identity, so package/list ordering cannot select a
different theme. The public `Theme::RegisterPackageTheme` and
`UnregisterPackageTheme` functions provide the same explicit composition boundary
for host-compiled modules; they require the GUI owner lane.

## Settings pages

Use `HoroEditorPageDescriptor` from `EditorPageAbi.h` with an
`editor.settings_page` manifest claim. Define the complete typed form/field
schema and localized messages. `EditorPageHost` owns drafts, validation,
cancellation and durable publication. Provider callbacks receive copied draft
values and approved semantic actions, not the GUI or canonical document store.
These pages appear in Settings' extension pages section.

Builtin source typography is under **Code Editor → Appearance**. Viewport input
preferences remain in Editor Input. New builtins enter the existing action or
configuration schema; modules use the contribution contracts above rather than
installing global handlers or process-wide mutable service discovery.

## Extending builtin source preferences

`EditorSettings::codeEditor` is the canonical typed user preference group.
`CodeEditorBooleanSettings()` and `CodeEditorIntegerSettings()` expose inert
member descriptors. A new builtin adds its typed member, stable unique key,
bounds (for numeric values), and matching `settings.code_editor.<key>` label and
`.description` messages in both editor catalogs. The catalog supplies the
settings controls, JSON read/write, numeric validation and module configuration
contribution; the feature owner consumes the typed member. This avoids a parallel
string property store. Module-owned features continue to use the settings-page
ABI above, with their own validation and publication authority.

Code Editor Appearance includes font family, font size, line numbers, whitespace,
word wrap, scroll bar and source autosave delay. The executable populates
`Theme::Fonts::codeFamilies` before atlas construction. Font faces are owned by
the GUI atlas; each registry entry borrows regular/emphasis faces for that session.
The source adapter only selects registered faces and never discovers native fonts.
Bundled IBM Plex Mono is the portable fallback; the host adds available Menlo/SF
Mono on macOS, Consolas on Windows, or DejaVu/Liberation Mono on Linux. Host modules
can append preloaded families during composition, before the first GUI frame.
A missing saved family retains its identity and renders with the bundled fallback.

These additive public contracts preserve existing callers: an empty family registry
uses the original atlas faces, and missing JSON preferences use typed defaults.
`SourceWidgetFrame` appends borrowed preferences/family fields; existing aggregate
initializers retain their meaning. Public header ownership is unchanged.

Source autosave is opt-in (`0` seconds disables it). `SourceDocumentService` owns
per-session timers, resets them at each new revision, and admits at most one due
save per owner update through the same revision-fenced `Save` path as the toolbar.
There is no independent file writer or implicit overwrite approval. Disk conflicts,
read-only files and failed/uncertain publication keep dirty text and back off for
30 seconds. Close destroys the timer; disabling resets pending timers. Idle
updates do no I/O and do not rescan unchanged source bytes. A due save executes
synchronously on the owner lane, like explicit Save; it is not background I/O.
Scene recovery autosave remains the separate minutes-based General preference.
