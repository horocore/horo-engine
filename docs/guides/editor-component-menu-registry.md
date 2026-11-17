# Add Component menu registry

The Inspector owns an `AddComponentRegistry`. It is an editor presentation catalog,
not a replacement for `Gameplay::ComponentRegistry`, behavior schemas, or scene
serialization. Built-in entries point to canonical `ComponentType` commands,
detached Physics creation intents, or existing source-creation workflows. Project
script entries carry registered `BehaviorTypeId` identities only.

## Static definitions

Definitions live in
`src/editor/screens/workspace/panels/inspector/add_component/definitions/`.
Each component has its own header containing a stable ID, localization key,
category, typed action and a pure availability predicate. The composition list is
`BuiltinAddComponentDefinitions.cpp`; compile-time validation rejects empty or
duplicate built-in identities. Reading a definition does not register services,
create entities, select a backend or invoke a callback.

To expose another supported component:

1. Add its metadata header under `definitions/`, using the existing typed scene
   action and editor label key.
2. Include it and add its definition to the composition array.
3. Add new copy to both editor localization catalogs if necessary.
4. Cover its availability and action in `AddComponentRegistryTests.cpp`.

`RegisterStatic` also supports explicit host-composed metadata contributions. It
copies metadata strings, rejects invalid/duplicate/reserved IDs and borrows only
static availability code. There is no global auto-registration. Adding a new
runtime component still requires its canonical model, serialization and editor;
this registry does not invent a second field schema or entity format.

## Dynamic project scripts

`ProjectGameplayRegistry` remains the authority for source discovery and behavior
registration. Its controller projection publishes `availableBehaviors` and an
`availableBehaviorsRevision`. The menu copies names, type identities and
multiplicity only when that revision or project scope changes. Unchanged frames
retain the same entries. Deleted/renamed/reloaded scripts replace the old
projection; changing projects or detaching releases old project contributions.
Duplicate dynamic identities fail the projection, and the Inspector does not
draw a failed projection.

Project `.horo_script` sources keep their existing `ProjectLayout::ScriptsRoot`
location. The new definition folder organizes engine editor metadata, not files
inside user projects. Native and Lua behaviors use the same registered metadata.

## Presentation and execution

`AddComponentMenu.cpp` draws registered categories/entries generically. It resolves
localized keys from the current locale and displays script names literally.
Availability reads the current object, locks, duplicate attachment policy and
Physics draft capacity; it is never cached from a previous selection.

Selection returns a typed intent. `ApplyAddComponentAction` translates it into
existing editor commands or the panel's detached Physics draft. The menu never
publishes document state or activates native Physics. Existing command validation,
Apply/Discard, schema validation and module implementation lifetimes remain with
their original owners. Built-in field rendering is described below; extension inspector schemas remain
owned by the existing extension host.

## Typed Inspector field definitions

Built-in primitive fields live beside the menu catalog in
`panels/inspector/component_fields/definitions/`. `Schema<Model>` is the typed
presentation registry; its `fields` array is checked at compile time for duplicate
IDs, missing labels/adapters/default providers and invalid choice lists.

Each field declares:

- stable UI identity and editor localization key;
- control type (number, integer, unsigned integer, boolean, text, choice, vector or color);
- a typed member pointer/accessor, including nested component members;
- display scale/format/speed, optional visibility and validation predicates;
- optional read-only presentation using a disabled shared input.

For example, another scalar on an existing typed model needs only a declaration
in its field array:

```cpp
Bind<MyComponent, &MyComponent::speed, Control::Number>(
    "movement_speed", "workspace.inspector.movement_speed")
    .Number({.speed = 0.1F})
    .Valid(+[](const MyComponent &value) { return value.speed >= 0.0F; })
```

Add the localization key to both catalogs. The generic renderer then draws the
field in declaration order. `Visible` can select current-model branches and
`Options` maps explicit enum values to localized labels. Changing a control type
requires a compatible C++ member; there is no string property path or untyped
payload map. Numeric degree presentation uses a display scale while retaining
radians in the model.

Defaults are read on demand through the accessor from `Model{}`. Define a new
field's default once in its canonical component initializer, rather than copying
it into editor metadata. Card Reset uses the same canonical constructor while
preserving enabled state and stable identity/bindings where required by that
component. Drawing never applies defaults to an existing attachment. Default
snapshots are intended for tooling/tests, not per-frame construction.

The drawer edits only the existing detached component draft and combines change
and commit events without skipping later fields. Existing edit sessions still
own command validation, preview/cancel, undo and document publication. Physics
continues to use its detached Apply/Discard transaction. Metadata validation is
not a substitute for those command/schema checks.

Composite asset/identity selectors, optional crouch geometry, Physics source/mass
variant transitions and repeated material/endpoint groups retain their specialized
typed adapters. Their transaction logic is not expressed as scalar setters.
Script field names/types/defaults continue to come from `BehaviorDescriptor` and
extension inspector/property-drawer contributions continue through
`EditorInspectorHost`; no second script schema or module callback registry is
introduced here.
