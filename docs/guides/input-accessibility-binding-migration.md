# Accessibility binding profile migration

ACC-001.4 extends the existing Input-owned `InputBinding` with a trailing
`BindingAccessibilityPolicy`. Digital policy is no longer a separate desired
preference store: the binding carries toggle, hold and repeat policy plus the
sticky-modifier choice. `DigitalAffordancePolicy` remains in `Horo::Input` and is
declared by `Input.h`; including `InputAccessibility.h` still exposes it through
the existing include relationship. Raw physical snapshots are unchanged by this
binding-model change.

### Gyroscope rate bindings

The same Input-owned model also appends `BindingControlKind::GyroscopeAxis`,
`GyroscopeBindingPolicy` and the `InputBinding::gyroscope` field. Existing control
and modality numeric identities are preserved; `InputModality::Motion` is appended.
Rebuild callers because `InputBinding` layout changes. `Input.h` remains owned
exclusively by HoroInput; no native/backend dependency or alternate profile store
is introduced. The staged header and explicit accessibility consumer cover the
new type. Existing neutral bindings require no source edits.

A gyro binding projects the existing platform `SensorKind::Gyroscope` sample into
an analog action. `SensorAxis` selects native device X/Y/Z without guessing a
camera, display rotation or gamepad/player assignment. The native platform owns
sensor enablement, permission and lifecycle. Only Available, finite SI samples
with a positive native timestamp contribute. Unsupported, disabled, pending,
suspended and failed samples contribute zero. This change does not create native
sensor availability; the existing Android sensor collector is a producer, while
SDL controller motion collection and hardware qualification remain separate work.

`radiansPerSecondAtFullScale` is finite in inclusive [0.001, 1000]. Values at or
beyond that magnitude saturate to a signed unit rate, followed by the ordinary
deadzone, `scale` sensitivity/inversion and action component. For example 1 rad/s
with full scale 2 and scale 0.5 produces 0.25 before final action aggregation.
This is a stick-like rate, not a mouse delta or integrated angle. Gameplay retains
tick-time integration; neither frame numbers nor unrelated native clock epochs
become durations. Reads allocate no storage and activation edges share the
frame-local consumption/rollback rules.

The unreleased canonical version-two vocabulary adds a gyro-only object:

```json
"gyroscope": {"axis": 1, "radiansPerSecondAtFullScale": 2.0}
```

It is required for GyroscopeAxis, forbidden for other kinds and rejected in
version one. Its two fields are exact: axis is an integer 0..2, calibration is a
finite bounded number, and unknown/missing fields fail. Control kind is checked
before byte-enum narrowing, rejecting negative, wrapped, boolean or floating
aliases. The ordinary serializer, atomic save, parser, merge and live SetProfile
remain the persistence/install path. Older version-two readers lacking this new
control must reject it rather than silently drop it. Digital actions and enabled
digital affordances cannot use a gyro binding. Duplicate or conflicting physical
gyro axes remain conflicts despite different calibration, scale or output component.

Physical identity comparisons also ignore fields inactive for the selected kind:
changing `key` on a gyro/gamepad/wheel binding, for example, never creates a new
physical control. The same identity helper governs duplicate and chord/sticky
overlap checks. Profiles that previously used inactive-field differences to hide
duplicate bindings are now rejected; remove the duplicate or assign a genuinely
different control. Rejection preserves the current map/profile and revision.
The all-kind identity/sticky regression passed in the 95-case native rerun below.
Analog axis exclusivity also remains in force when either binding has a chord:
chord keys may be held together and cannot grant two actions the same physical
axis. Exact, mutually exclusive ordinary modifier states remain separate modes.
The regression covers gyro, gamepad/raw axes and both wheel axes with chords,
and checks that different modifier modes are still admitted.

Chord identity now uses the same held-key set as routing. Reordering chord keys,
repeating an existing key or changing storage beyond `chordSize` cannot disguise
two bindings for the same transition. Such profiles are rejected atomically;
remove the redundant binding instead of reordering its chord. This changes no
wire fields or raw input state. The new regression checks map/profile rejection,
unchanged revision and preservation of a genuinely different chord. It is not
included in the historical 95-case pass below; its native rerun is pending.

The native MSVC Debug batch at `2b2175ca9847ca97dd50d648563a3cafbc2f82bd`
built both Input test executables and staged/explicit public consumers, then passed
**95/95 cases with zero skips** in 1.47 seconds. This includes all six added
gyro/router/profile cases and the extended atomic save/load case with spaces and
a non-ASCII path, plus the all-kind identity/sticky/chord regression. Reports are
`build/acc-input-identity-2b2175c-results.xml` and its
discovery JSON. The earlier 88-case Input pass predates the extension.
A subsequent editor change
adds explicit Add/Edit commands routed through the existing exclusive modal host.
Its owned numeric draft preserves full float precision and invalid intermediate
text; axis, component, calibration and sensitivity reach the same SetProfile
authority only on Apply. Revision plus an exact replacement baseline rejects
retargeting, append preserves all existing bindings, and Cancel/close writes
nothing. All six model/modal/render cases ran in the **98/98** GUI/SDL batch at
`2b2175ca9847ca97dd50d648563a3cafbc2f82bd` (54.04 seconds, zero skipped), alongside
the actual editor and staged GUI header builds. Its retained report is
`build/acc-gui-identity-2b2175c-results.xml`. Paired-copy/CI Python checks passed
**43 tests** after the final source fixes. This includes the physical identity,
chord exclusivity, error-free frame validity query and strengthened modal-gating checks.
Native interaction, automatic motion-gesture capture, SDL controller collection
and actual device evidence remain separate acceptance work.

The subsequent direct-consumer batch at `d460d27` rebuilt eleven targets and
passed **62/62 cases with zero skips**: Character 16, Runtime UI input 19,
debugger 15, portable Android input 9 and three direct consumers. Staged
Character, RuntimeUiInput, AndroidInputIntegration and XRInputBindings headers
compiled against the current Input surface. Evidence is
`build/acc-input-consumers-d460d27-results.xml` and its discovery inventory.
Offline SDK metadata compatibility also passed **24/24** at that commit;
neither check establishes physical sensor or native Android qualification.

## Wire and source migration

`CurrentInputBindingProfileSchema` is 2. A newly constructed profile and every
writer use version two. `ParseBindingProfile` accepts a legacy version-one file
without accessibility fields and returns the canonical version-two in-memory
model with neutral policies. It does not rewrite that file during a read; the
normal persistence owner saves the new version through the existing atomic writer.
There is one current serializer and one current in-memory model.

Version-one documents containing accessibility fields are rejected instead of
silently dropping those fields. Unsupported versions and floating version numbers
are rejected before narrowing. Code constructing an explicit version-one
`InputBindingProfile` must migrate to the current constant; merge, validation and
serialization admit only the canonical in-memory version. Code reading an old
file should use the parser rather than copy its wire version into a new model.

The version-two binding contains this optional object; omitted fields use neutral
or documented repeat defaults. The canonical writer emits the complete object:

```json
"accessibility": {
  "toggle": false,
  "stickyModifiers": false,
  "holdMicroseconds": 0,
  "repeat": false,
  "repeatDelayMicroseconds": 400000,
  "repeatIntervalMicroseconds": 100000
}
```

Booleans are exact JSON booleans. Durations are exact non-negative integer
microseconds bounded to 60,000,000; repeat interval is positive even when repeat
is disabled. Negative, floating, string-coerced, oversized and unknown policy
fields return typed malformed-profile failures. Serialization rejects invalid
policy values instead of producing a file the parser cannot consume.

New project scaffolding calls `SerializeBindingProfile` for `.horo/input.json`;
it no longer embeds an independently maintained version-one JSON literal. The
existing project creation test inspects the generated file through the ordinary
Input consumer and checks its canonical version, identity and empty overrides.
Editor preference document versions are owned by their existing format and are
not changed by this input-profile migration.

## Validation and consumer impact

`ValidateBindingProfile` remains the sole binding conflict admission path. Policy
does not make duplicate physical transitions distinct. Non-neutral affordances
require a digital action. Sticky versus physical bindings can overlap even with
different required modifiers: physical modifiers may be a subset of the sticky
chord after sequential latching. That overlap is rejected within one action and
across actions in the same context. Two different exact sticky chords do not
become aliases merely because both use latching.

Existing source using neutral/default trailing fields remains valid. The C++
`InputBinding` layout changes, so affected HoroInput consumers must be rebuilt;
binary compatibility across this development-branch change is not asserted.
Public staged header consumers cover the Input-owned types. No new target or
cross-target private-header dependency was introduced; the new codec helper is
private to HoroInput, and EditorServices already depends on Input.

## Qualification status

The canonical raw snapshot now also carries optional `InputSampleTime` metadata in
integer microseconds. `RawInputCollector::BeginFrame` and `InputService::BeginFrame`
accept that explicit host value; the existing one-argument call remains source
compatible through a default unavailable argument. Typed pointers to those member
functions must adopt the new signature, and C++ consumers must rebuild.
Every BeginFrame replaces the metadata, so an unavailable time never reuses an old
timestamp or interprets a frame ordinal as elapsed time. Equal timestamps are
valid, and physical key/button facts are not synthesized by time publication.

SDL's platform adapter supplies `SDL_GetTicksNS()/1000` at the actual collection
boundary. The terminal simulator workflow supplies the runtime owner's admitted
presentation duration instead, making its selected logical/replay time explicit.
An invalid negative runtime duration yields unavailable metadata rather than an
unsigned wrap. Mapping itself performs no system-clock call. New regression
sources cover legacy/no-time collection, equality/maximum timestamps, clearing
missing time, the canonical action-collection commit and native SDL publication.
These collection checks alone do not qualify timed router consumption or device hardware.

Regression sources cover legacy read/current write, policy round trip, layered
profile replacement, malformed numeric and boolean types, unknown fields,
duration bounds, version skew and sticky/physical conflict detection. The focused
MSVC Input batch compiled and ran these cases as part of its 88/88 pass at
`dcf772e`; native SDL publication and editor GUI checks remain separate.

## Router ownership and source migration

The existing `InputRouter` now applies each enabled policy after canonical physical
interpretation and routing admission. State is keyed by exact context incarnation,
typed action-name lease, binding index and optional player. Held device mappings
remain pinned to the exact device generation. Raw snapshots are never rewritten;
already semantic action-collection sources retain their existing interpretation.
Multiple binding values keep the router's existing OR aggregation of digital facts.

Hold and repeat use only the host's explicit sample time. Missing time refuses the
whole timed action with `ActionReadStatus::ClockUnavailable`; reverse time retires
old state. Repeat is the trailing `ActionValue::repeated` field, separate from a
fresh activation's `pressed`. The router advances accessibility state at most once
per committed sample, so repeated reads preserve held state without replaying
synthetic edges. These public layout and enum additions require consumer rebuilds.

`DesiredMotionAdapter::CaptureInput` now preserves missing host time as
`Input::InputAccessibilityErrors::ClockUnavailable` instead of incorrectly
reporting Character capacity exhaustion. It clears all pending movement/jump
intent on that failure. Its public return contract is broadened by this specific
typed Input error; callers must preserve it when presenting capture failures.
This adds no Character authority, dependency or callback ABI. The regression
source verifies cause, neutralization, recovery and a fresh complete hold after
time returns; native execution remains pending.

Focus loss, context preemption/removal, accepted configuration replacement,
device disconnect/reassignment and reversed time retire the old incarnation.
Rejected candidates preserve the active map and state. Live owners drain their
pending release through their original token and action name, even while
preempted or after the name is removed from the map. An ended context emits an
owned `RetiredInputActionRelease`, drained through `TryDequeueRetiredActionRelease`
at the host's ordinary input/owner cutoff. Matching old context/action/player
withdrawals coalesce; they never invoke dead callbacks or become replacement-context
input. The host must route these records to its existing teardown authority;
that application composition is still pending. The immutable action-name lease
may outlive the router. Shutdown must drain before destroying that owner.

Setup prepares identity storage before publishing a configuration. Mapping reads
use a fixed 1,024-slot inventory and at most 64 effective bindings per opted-in
action with an action-name limit of 256 bytes. Pending releases retain their slot
until drained. Capacity refusal rolls back this action's physical consumption,
candidate state and sticky consumption. `EndFrame`, or the next committed sample
for hosts without that call, reclaims drained identities at an owner boundary.
No action read allocates name storage, polls a clock or invokes a callback.

New/reset mappings require observed neutral input before a fresh press. Sticky
modifiers are context-owned and consumed only after a successful admitted chord;
physical modifiers remain raw facts. Focus, configuration, context and device
inventory/assignment boundaries reset the latch. An admitted hold retains its
chord until physical release, without granting another fresh press the used latch.
Routing and capture identities now stop at exhaustion instead of wrapping;
`PushContext` returns an empty token and `CapturePointer` returns a typed failure.

Regression sources exercise actual router reads, repeat timing, skipped reads,
modal/focus/time cancellation, accepted and rejected rebinding, scoped name
lifetimes, ended-owner coalescing, device/player generations, sticky reset,
capacity rollback, owner-boundary reclamation and allocation-free action reads.
The focused MSVC Debug Input batch at `dcf772e` compiled and ran both Input test
families plus the explicit public consumer: **88 passed, zero skipped**. See the
delivery qualification guide for the exact scope and earlier failed run.
The existing Input Mapping panel now exposes an explicit per-binding accessibility
draft for digital actions. It uses shared controls and paired English/Turkish copy,
without introducing another profile file or settings authority. Millisecond text
accepts dot/comma decimals with at most three fractional digits and preserves
integer microseconds exactly. Invalid/incomplete text stays visible, Apply is
disabled, and Cancel leaves the profile unchanged. Duration bounds and a positive
repeat interval remain enforced even for inactive fields.

Apply checks the original exact configuration revision and full binding before
copying the current effective profile. A stale draft, removed/replaced binding or
non-digital action cannot publish. The ordinary `SetProfile` performs conflict
admission and lifecycle cancellation; peer bindings and action defaults remain
intact. Existing profile save/reload controls persist these same version-two values.
The edit form wraps localized labels without shrinking typography. The existing
deadzone edit returns after profile replacement so its old binding references are
not read after invalidation.

Model and headless GUI regression sources cover exact decimal round trips,
invalid fields, bounds, stale generation/ABA rejection, peer preservation,
canonical codec round trips and sticky conflicts, plus paired locale/narrow form
drawing. These C++ tests have not compiled or run. The paired localization and
SDK Python pass is now **26 passed**, which does not qualify native behavior or
visual interaction. Input workstream rebinding capture/persistence integration,
additional host withdrawal composition and gyro remain implementation/qualification work.
This checkpoint does not qualify ACC-001.4.
The graphical editor now drains removed-context withdrawals at its actual input
cutoff, after the router adopts the committed sample and before screen input
capture. It releases only ended-owner name leases and never delivers them to a
replacement context or invokes a removed consumer. Live/preempted contexts retain
their ordinary release reads. The new bounded lifecycle regression repeats more
than the complete mapping capacity through context replacement without an explicit
router `EndFrame`, then checks that host draining cannot steal a live modal
cancellation. This model regression passed in the 88-case Input batch; the
graphical application TU and other compositions still need qualification.

`GameplayInputFrameBuilder` now clears all held and pending commands when any
routed action reports `ClockUnavailable` or `CapacityExceeded`. Previously its
value-only reads could retain a jump/interact edge from an earlier zero-tick
frame through a failed capture. Capture's call signature is unchanged; the new
`LastCaptureStatus()` getter exposes the exact failure without allocating a
diagnostic in this frame path. Duplicate capture preserves the outcome, and
`Reset()` returns it to `Unavailable`. Optional absent actions remain neutral.
Rebuild consumers of the changed public builder layout. The existing staged
accessibility consumer now exercises this getter; regressions cover missing
clock, retained-status/duplicate capture, complete neutralization, fresh hold
after restoration and mapping-capacity failure. These cases and the consumer
passed in the focused 88-case native batch at `dcf772e`.
The separate `HoroCharacterInputTests` family and its staged/public consumer built
at `6308740`; all 16 cases passed, including missing-clock cause preservation,
pending-intent cancellation and jump repeats across zero/catch-up tick frames.
Input's independently owned scoped persistence/capture work must be
integrated without replacing its strengthened validation or adding another save
path. That work and this version-two change are published in separate draft PRs
[#3550](https://github.com/horocore/horo-engine/pull/3550) and
[#3560](https://github.com/horocore/horo-engine/pull/3560); integration conflicts in
the common codec must preserve both contracts.
