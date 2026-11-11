# Accessibility binding profile migration

ACC-001.4 extends the existing Input-owned `InputBinding` with a trailing
`BindingAccessibilityPolicy`. Digital policy is no longer a separate desired
preference store: the binding carries toggle, hold and repeat policy plus the
sticky-modifier choice. `DigitalAffordancePolicy` remains in `Horo::Input` and is
declared by `Input.h`; including `InputAccessibility.h` still exposes it through
the existing include relationship. Raw physical snapshots are unchanged by this
binding-model change.

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
They do not yet qualify timed router consumption or device hardware.

Regression sources cover legacy read/current write, policy round trip, layered
profile replacement, malformed numeric and boolean types, unknown fields,
duration bounds, version skew and sticky/physical conflict detection. They have
not yet compiled or run under the allocated shared native build slot.

Router consumption, explicit monotonic hold/repeat time, synthetic cancellation
release ownership, rebinding UI exposure and gyro support still require their
integrated implementations and tests. This wire/model checkpoint does not qualify
ACC-001.4. Input's independently owned scoped persistence/capture work must be
integrated without replacing its strengthened validation or adding another save
path. That work and this version-two change currently live in separate unpushed
worktrees; merge conflicts in the common codec must preserve both contracts.
