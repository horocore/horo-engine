# GUI localization scope migration

Modal, panel, and screen localization previously depended on repeating complete
catalog prefixes at call sites or concatenating them in individual helpers.
The shared `LocalizedGui` contract now requires an explicit scope at construction.
This is an intentional source and object-layout change for every implementation
of `EditorModal`, `IWorkspacePanel`, `IGlobalDockPane`, and `GuiScreen`, including
test doubles. Embedded dock panes and UI inspectors also adopt `LocalizedGui`
directly, so their scopes do not depend on how a host adapter mounts them.

## Surface implementations

Define the prefix once and pass it to the direct surface base constructor:

```cpp
static constexpr GuiLocalizationScope kTextScope{"workspace.gameplay_behavior.create"};

GameplayBehaviorFilenameModal::GameplayBehaviorFilenameModal(/* existing inputs */)
    : EditorModal(kTextScope) /* existing initializers */ {}
```

Resolve `"title"` or `"invalid_filename"` through
`LocalizedText(context.localization, key)`. View helpers use the owning
`kTextScope.Text(context.localization, key)` definition. A surface spanning shared
vocabulary can declare a separately named scope for that vocabulary; it must not
guess whether a key is already qualified. Test doubles explicitly use a test
scope. Empty scopes and prefixes with boundary dots fail constant evaluation.
Prefixes are bounded to 128 bytes and copied into the scope value, so no stored
view points into an unloaded live-UI DLL. Successful lookups require no prefix
allocation; only missing-message diagnostics enter the existing missing cache.

The surface base classes no longer have implicit default constructors.
Retain each class's existing initialization, lifecycle and close policies while
adding the required base initializer. Restart the editor after rebuilding: this
base-layout migration cannot be applied safely to a running host by UI live reload.

## Localization implementations and header consumers

`ILocalizationService::GetScoped(namespaceId, scope, key)` resolves the same
canonical catalog identity as `Get(namespaceId, scope + "." + key)`. Existing
adapters inherit a delegating implementation. `LocalizationService` overrides it
with heterogeneous segmented lookup, and also uses that lookup for `Get`.
`LocalizationCatalog::messages` now declares transparent `MessageKeyHash` and
`MessageKeyEqual` policies; callers continue inserting owned `MessageKey` values.

The catalog format, missing-key identity, locale activation boundary and existing
namespace separation are unchanged. Deferred message identities from models and
extension-provider localization remain canonical external inputs, rather than
being reinterpreted as surface-relative keys.
`AssetImportModal::Localized` now takes a relative asset-import key; importer-owned
complete keys use `LocalizedCatalogMessage` explicitly. Public build-status
presentation keys and network-state keys retain their complete catalog identity
for external consumers.
Rebuild native consumers of `ILocalizationService`, whose vtable adds the scoped
lookup entry, in addition to rebuilding surface implementations.

`LocalizedGui.h` is owned by `HoroEditorServices` in the public-header registry.
Consumer compilation covers the common contract through all public surface
bases. Regression coverage verifies explicit scope construction, active locale
changes, canonical/segmented lookup equivalence, missing identities, segment
boundaries, and zero observed C++ allocation requests for successful repeated
lookups. Existing modal, panel, route, and localized render tests validate migrated
callers.

Windows workspace tests explicitly compile their Unicode source fixtures as
UTF-8, and the PCG fixture converts its canonical UTF-8 source identity into a
native filesystem path before writing it. This keeps document-opening failures
from masking the GUI migration tests.
