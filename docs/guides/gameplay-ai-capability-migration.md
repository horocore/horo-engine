# Gameplay and script AI capability migration

HORO-1317 / #1317 / [GAI-001.9]

## Ownership and composition

`HoroAI` owns inert bounded task declarations. Registration copies stable task IDs,
authoring origin and an optional required blackboard schema; it does not resolve a
runtime, register a provider, invoke callbacks or activate work. The native module
host freezes these declarations before startup.

`HoroAISceneIntegration` owns `AiRuntimeCapability`, issued by the actual active
`AiSceneRuntime` on its simulation safe-point thread. It retains a weak reference
to one exact publication, not a runtime pointer. Disabled/retired agents, stale
handles, publication replacement and shutdown fail closed. Stable `AgentId` values
are resolved only within that publication; runtime handles are never durable IDs.
The issuing thread and publication owner are checked before mutable access.

`HoroGameplayAiIntegration` composes a permission-admitted `GameplayAiContext`.
Only application composition calls `Create` with an owner-issued runtime
capability and explicit module policy. The context is not a backend selector.
`GameModuleHost` creates a separate private child per module generation, binds
its frozen metadata, and retains it independently from mutable
`GameRuntimeContext::ai`. Project modules receive only a const context; they have
no runtime pointer, underlying capability getter, provider constructor or public
child-derivation method. Missing context is reported by `AcquireGameplayAi` as
`AIErrors::CapabilityUnavailable`.

The canonical constructor is `GameModuleHost(GameModuleHostOptions)`.
`GameModuleRuntimeServices` contains explicit Physics and AI bindings. The old
vector and vector/Physics constructors thin-forward to those options. Options
require both constructor arguments, preserving the meaning of `GameModuleHost({})`
and `GameModuleHost({}, {})`. Existing callers may migrate when they compose AI;
callers without AI retain a recoverable missing-capability path.

## Task and blackboard authority

Native and script task operations use the existing `AiTaskLifecycle` and exactly
the same Scene-owned task slot, cancellation precedence and terminal outcomes.
Module scopes retain a bounded ownership index only; another module cannot operate
on a task by copying its handle. Scene/task and module grant cancellation are
retained separately, with copied operation lineage. No module function table,
virtual runner, VM reference or executable callback is stored in these tasks.
Terminal release retires task observers before releasing the existing task owner.

Blackboard reads copy one authorized value without allocating a whole snapshot.
Writes use the existing revision-fenced `BlackboardWriteBatch` and atomic
`BlackboardSync` commit. Schema, type, read-only, authorization and stale-revision
checks remain at the existing owner. AI publication revisions also fence these
mutations against prepared restore transactions. Admission/registration is bounded;
task creation and copied script values may allocate, while ordinary task resume
continues to use the existing lifecycle state machine.

`HoroGameplayAiScriptBridge` is downstream of Gameplay, AI scene integration and
Extensions. AI core has no reverse dependency on Gameplay or a VM. The bridge
uses `ScriptCapabilityBinding::Begin`, existing invocation controllers and existing
registry-issued opaque handles. The added binding handle methods forward to the
same exact context/provider authority; release remains possible after scope
revocation so cleanup cannot leak live registry resources.

The version-1 `horo.gameplay.ai` descriptor exposes agent resolution, task
start/state/cancel/complete/release and blackboard read/atomic writes. Blackboard
values use explicit typed logical fields for exact signed integers, finite
scalars, stored references, millimeter coordinates, bounded homogeneous collections
and preserved opaque values. Native object memory, padding and addresses are never
serialized. Script operation diagnostics come from the admitted binding, not VM
supplied identity fields.

## Canonical script identities

The new version-1 script surface uses canonical lowercase, dot-separated identities
with hyphens inside segments. Functions include `resolve-agent`, `start-task`,
`task-state`, `cancel-task`, `complete-task`, `read-blackboard` and
`write-blackboard`. Logical projections use `ai-scalar`, `ai-value`, `ai-write`,
`element-kind`, `opaque-version` and `opaque-type`. Script/Lua callers must use
these names; the earlier unmerged underscore spellings never admitted a descriptor
generation and are not retained as aliases. The common descriptor validator is
unchanged.

Native AI error descriptors remain unchanged. A closed, explicit mapping declares
the script error identities, for example `ai.scene.capability-unavailable` for
native `ai.scene.capability_unavailable`. Failure transport preserves the native
domain, message and copied cause details, and includes the original exact code in
`native-code` evidence. Descriptor declarations and invocation failure transport
share that same mapping. An unmapped native error becomes declared `ai.native-error`
with its original domain, exact code and cause evidence retained; no arbitrary
error identity is normalized. Native callers
continue to receive the original typed AI error.

## Reload and image lifetime

Module cancellation and AI revocation occur before native prepare-reload/stop
callbacks. The host retires exact owned task resources on the simulation safe point
and clears exposed context fields before unloading the native image. Replacing a
module does not revoke its host anchor or a later independently derived generation.

Script reset and provider/scope retirement immediately close invocation admission.
The script owner calls `ReapRevokedAtSafePoint` before the next AI decision phase
and before destroying its bridge; this cancels and releases existing Scene-owned
tasks without invoking script or module callbacks. Destruction retires remaining
opaque projections. Unpublished resource admission is rolled back if invocation
completion loses to cancellation. Blackboard commit is the operation's mutation
linearization point; later cancellation can suppress delivery without undoing an
already committed atomic write.

The native module allocator hazard is checked at the actual API edges. A native
module receives a host-created const holder, cannot obtain the underlying runtime
capability, and cannot invoke private child construction. Public `Create` is a
composition-root factory, not client derivation; neither the host nor a Scene weak
registry adopts a holder manufactured by module code. The host ignores changes to
the mutable exposed field for authoritative ownership. The Scene registry owns
plain nonvirtual task objects and copied values, not weak `GameplayAiContext`
control blocks or module deleters. Script resource records retain existing
host-issued binding/handle values and typed AI IDs; they introduce no callback
or allocator dispatch supplied by a module. An actual native image fixture retains
host clients across image unload and destroys the last strong and weak clients
afterward. This differs from APIs that allow a module to derive a shared client
registered weakly inside the host; those require immutable host-issued allocator
dispatch rather than merely copying `shared_ptr` values.

## SDK boundary and integration

The current combined Navigation and AI layout uses reserved gameplay boundary
**18** and `gameplay-sdk18`. Rebuild native modules, generated descriptor bundles,
runtime hosts and public consumers together. Earlier independent AI11, NAV10,
combined NAV+AI16, Character15 and Runtime UI17 are distinct incompatible layouts.
A future union requires a newly allocated epoch; taking the maximum existing
number does not establish identity. The C ABI extension version remains separate.

## Additional native module fixture metadata

The CMake helper now accepts optional `INPUT_MANIFEST_OUTPUT`. Existing callers
retain the primary project default `.horo/local/gameplay_build_inputs.txt`, which
the build service uses for canonical freshness hashing. `NO_MANIFEST` continues
to suppress only runtime artifact JSON; descriptor bundles, revision files and
fingerprints still exist. The additional AI native fixture explicitly owns a
separate input manifest inside its binary directory, preventing two modules from
generating different contents at the same project-global path. No existing caller
migration is required. A real two-module configure regression checks both outputs
and the two retained descriptor/revision generation commands.

## Verification scope

Regression sources cover native/script lifecycle parity, typed missing and denied
capabilities, foreign task ownership, revocation/cancellation, exact publication
replacement, disabled agents, atomic blackboard failure, full scalar/collection
projection, real native module unload and legacy brace constructors. A real
public consumer and owning generated header consumers belong to the Windows
build/discovery closure. Authored tests are not evidence of execution; local and
hosted results are recorded separately in the delivery PR.

## Native fixture qualification

The actual AI module statically links the two AI integration owners. Both join
the existing host-only position-independent Scene/AI static closure so Linux can
link the native image; this does not expand the exported Physics SDK.

`HoroGameplayModuleInputManifestTest` is an owning CTest external-project configure
qualification, selected on Windows and in the ordinary Linux/macOS suite. Each
run creates fresh source/build paths containing spaces and configures two actual
gameplay module targets. It checks the primary default freshness manifest remains
unchanged, the secondary owns its distinct manifest, and both retain generated
descriptor bundles, revision paths, module IDs and SDK fingerprints. Python tests
only verify metadata registration; they no longer launch external commands.

The merged navigation runtime layout used boundary10. The previous combined Navigation
and AI layout used boundary16; the diagnostics representation below requires
fresh boundary18. Rebuild the host, all modules and generated bundles together. Existing
navigation-aware three-argument constructors forward into the canonical typed
options and retain their explicit navigation service; Physics and AI grants remain
separate fields. Character15 and UI17 reservations do not identify this layout.


## Allocation-free internal task context and SDK18

`AiTaskOperationContext::diagnostics` now holds an immutable owned snapshot
through `shared_ptr<const Telemetry::OperationContext>`; internal graph tasks leave
it empty. This prevents default/copy/move construction of an empty diagnostic
vector from allocating MSVC Debug iterator proxies during the existing allocation-
free traversal, restart and shutdown paths. Consumers of this newly introduced
field check the pointer before reading it. Task inspection retains the same immutable host-owned diagnostic holder,
without moving an owning diagnostic vector through Result. Inspect callers check
`diagnostics` before reading `diagnostics->operationId` or its immutable fields.
Both omitted-context and explicit-context StartTask overloads preserve existing
call syntax; omitted diagnostics never construct an owning default argument in
caller code. These changes preserve native/script lifecycle and diagnostic parity.

Original scene-state construction binds a const private capture function pointer
to the host's diagnostic copier. Admission invokes that retained host function;
module code cannot supply or replace it. `make_shared` copies the caller's
OperationContext in the original host image before publishing any task handle,
state revision or module ownership record. The context then moves only a
shared_ptr into the lifecycle. This also removes a concrete failure path where
MSVC's noexcept vector move constructs an allocating iterator proxy, bypassing
the caller's bad_alloc recovery. The original exhaustive measured fault-offset
loop and no-allocation traversal oracle remain unchanged. Source diagnosis is
separate from fresh Windows execution evidence.

The snapshot's control block and deleter remain host-owned. Module retirement
revokes its scope and releases tasks before image unload; returned task facts are
owned values. Internal/default task contexts do not allocate a diagnostic owner,
and no mutable provider, runtime pointer or callback setter is added.
