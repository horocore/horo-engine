# Activity and drawer contribution migration

## Required boundary

Activity destinations and drawer content are host-rendered extension surfaces.
Extension code publishes copied typed metadata and a bounded `EditorUiForm`;
it never receives `IWorkspacePanel`, ImGui, draw lists, docking IDs, native
window handles, renderer resources, or the editor service registry. Descriptor
validation is inert. Registration, activation, placement and rendering are
separate explicit host operations.

The existing panel/tab `EditorSurfaceRegistry` remains the authority for
provider availability, open/focus intent and bounded workspace presentation
state. Activity presentation binds to a panel owned by the same extension,
module and activation generation. Provider revocation withdraws both the
activity route and its drawer before module shutdown. A stale route or action
cannot target a later activation with the same stable contribution ID.

## ABI compatibility plan

The activity provider's callback/context pair is retained only by a private
standard-layout C ABI adapter. Internal action execution uses that typed endpoint
and an owned result handoff. Actual host callback adapters have C language linkage;
provider invocation is a non-allocating `noexcept` boundary that translates both
standard and non-standard throws to `HORO_EXTENSION_ERROR_INIT_FAILED`. The job's
existing retirement lease still pins native code and context until it returns.
No ABI field, table size, version, prefix negotiation or C11 source spelling changes.
The activity registration alias uses `using` under C++ and `typedef` under C11;
both describe the same C callback signature.

`EditorActivityHost::QueueAction` is now const-qualified as a host-facade operation
on separately owned live sessions. Owner-lane affinity, admission and pending-command
mutation are unchanged; const does not authorize concurrent dispatch. Ordinary
callers need no source change. C++ member-function-pointer consumers must add the
const qualifier, and statically linked C++ consumers must rebuild for the changed
symbol. This internal host API is not part of the installed C11 extension ABI.

The current native host ABI is 1.3. The activity/drawer bridge appends the
versioned editor contribution entry point after the existing host API prefix
and increments the host minor version to 1.4. Existing 1.0–1.3 modules retain
their exact prefix layout. New modules must declare minimum ABI minor 4 and
check `structSize` before reading the appended field. The host advertises the
canonical `editor.activity` host capability only when the graphical composition supplies
it; a headless host exposes no editor registration entry point. The host capability identifies the ABI 1.4 registration/session transport; it
is distinct from the `editor.activity_item` and `editor.panel` contribution
claims. Named contribution-point segments admit internal underscores (for
example `activity_item`); package/module/capability IDs retain their existing
canonical grammar. Syntactic parsing grants no point authority: the activity
registration path still requires exact manifest claims and host admission. Modules require `editor.activity` explicitly. Required
unavailable capabilities reject activation without silent fallback.

Borrowed ABI descriptors and form data are copied and validated while their
registration call is live. The loader checks the contribution against its
owning manifest and stages the complete package transaction before publishing
any editor surface. Rollback and unload destroy surface/session registrations
and revoke action admission before unloading the module library.

## Presentation and persistence

The workspace host owns side/bottom placement, activity ordering, visibility,
active destination, drawer geometry, keyboard focus, input exclusion and
accessibility. It renders the admitted form using shared design-system controls,
semantic theme roles and localization. Narrow drawers wrap or scroll without
reducing the visible typography tier. Provider code executes outside widget
traversal; rendering reads an immutable copied form snapshot.

Workspace restoration stores stable contribution and provider identities,
placement and bounded presentation intent. Activation generations, capability
grants, function pointers and native objects are never persisted. Missing or
disabled provider state remains bounded opaque presentation data; restoring it
does not reactivate a provider. Invalid, duplicate or oversized state is rejected
atomically. The existing workspace authority owns serialization rather than a
second activity-specific registry.

## Qualification

Required evidence includes old-prefix C11 consumers, unsupported host/schema
rejection, transactional package registration, side and bottom drawer admission,
one active destination per side, open/close/focus behavior, visibility and badge
updates, ordering, narrow geometry, workspace round trips, missing/disabled
provider restoration, stale-generation rejection and teardown before unload.
GUI qualification covers hover, active, disabled, keyboard focus, modal exclusion,
long localized labels and narrow drawers through the real host composition.

## Implementation boundary and outstanding qualification

The current change stages `EditorActivityAbi.h` in the source-free SDK and
publishes ABI minor 4 in its compatibility metadata. `ExtensionManager` appends
an optional `EditorActivityHost` composition parameter. `GuiScreenHost` appends an
optional native-artifact gate and composes that host with its application job
system. Existing callers compile with the omitted arguments and keep the
previous fail-closed native-load policy.

The production HoroEditor caller currently supplies no native-artifact gate.
There is no existing editor trusted-root/envelope configuration to reuse. The
normative plugin contract requires verified package bytes/resources and explicit
local trust (plugin-system sections Package Format and Trust and Permissions).
This change does not invent trust roots or infer trust from an enabled package.
Known trusted fixture gates exercise registration, actions, SVG resources and the
actual GUI cache; passing those tests cannot establish production trust wiring.

Static SVG profile version 1 supports absolute `M L H V C S Q T A Z` path
commands (and lowercase close `z`), finite coordinates with magnitude at most
1024, decimal tokens of at most 32 bytes and explicit exponents from −32 to 32,
256 commands, 2048 numeric operands, 128 elements, XML nesting depth 8 and
64 KiB of input. View-box dimensions must be at least 1 and intrinsic dimensions
remain between 1 and 1024. Output is always 48×48 owned straight-alpha RGBA8.
Relative paths, transforms, style sheets, text/fonts, images, scripts, entities,
external references, `use`, filters and masks are rejected. Local gradients have
no inheritance or recursion. These restrictions bound curve/subdivision and
pixel work as well as parser allocation. LunaSVG 3.5.0 and its vendored PlutoVG
1.3.1 are pinned at `83c58df8103dc7dca423dfd824992af94d49bed6`; the owning
Extensions target links them privately. SVG decoding occurs during registration,
texture upload/retirement at the GUI Update boundary, and Draw uses cached data.

The workspace serializer evolves to schema 3 with bounded typed surface entries
and optional user activity placement. Schema-1 and schema-2 layouts remain readable.
The registry workspace DTO evolves from schema 1 to 2; legacy entries use declared
provider placement. User overrides retain side, group and insertion ordinal across
unload/reload independently of immutable provider declarations. The GUI supports
activity drag/drop between rails and groups, plus localized keyboard-accessible
placement actions for left/right/bottom and ordering. Draw queues fixed-size,
projection-revision-bound commands; the owner Update validates provider generation
and publishes placement before refreshing retained controls. Revocation or an
intervening revision discards queued moves. A moved open destination closes any
other destination on its new side together with its paired drawer. Surface state uses the existing registry bounds:
512 entries, 8192 bytes each and 1 MiB total state. JSON expansion is capped at
8 MiB; invalid surface bounds reject Save before touching the previous file. The GUI maps those entries to the existing
registry's Save/Restore values in `.horo/editor_workspace.json`. It preserves
user visibility independently of provider availability, rejects inconsistent
paired state atomically, and keeps activation generations out of persistence.
The transport's node mask and state bounds are explicit; unsupported capabilities
or node kinds receive a rejection, never silent rendering fallback.
