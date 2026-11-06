# Particle-System Descriptor Migration

Particle authoring and import code must use the typed descriptor boundary instead of
passing permissive JSON, ad-hoc maps or backend-native render and collision values into
runtime code.

1. Persist the exact `ParticleDescriptorSchemaVersion`, stable `EmitterId` and
   canonical material `AssetId`. Numeric enum values and native renderer or physics
   handles are not source contracts.
2. Call `ParseParticleSystemDescriptor` with project-lowerable limits. Treat malformed,
   duplicate, oversized and unsupported-version results as typed operation failures;
   do not retry with a permissive parser.
3. Build one immutable error registry containing all particle validation descriptors,
   then call `ValidateParticleSystemDescriptor`. Present every returned diagnostic.
   Only the optional admitted descriptor may cross into cook planning.
4. Capture material evidence from the exact immutable asset snapshot owned by the cook
   composition boundary. Pass that evidence and an explicit canonical
   `ParticleCookProfile` to `BuildParticleSystemCookPlan`.
5. Publish cooked output only when the returned validation is accepted. Missing,
   mistyped or unloadable materials, tier overflow and incompatible policies are
   terminal typed cook findings; no implicit material, tier or execution-domain
   fallback is permitted.

Existing sources without `schemaVersion`, stable identities, finite bounds or an
explicit kill condition require an offline migration before import. Version 1.0 is the
only directly readable schema; future migrations must preserve the original source
until the new candidate validates and cooks successfully.

## Authored GPU observation policy

Source schema 1.0 now accepts an optional closed `gpuReadback` object, following the
existing optional `collisionResponse` extension. Absence constructs the canonical
`Render::VfxGpuReadbackDescriptor{}`: observations are disabled, the schema is one,
the purpose is diagnostics capture, cadence is one, and every storage limit is zero.
Existing sources therefore keep their observation-off behavior. This is an additive
reader change, not an invented source-version migration. Older closed readers reject
sources that contain the new field; preserve the original source when targeting them.

An enabled example observes at most 16 normalized records every third committed
logical step, with two pending results and 1,280 bytes per result:

```json
"gpuReadback": {
  "enabled": true,
  "schemaVersion": 1,
  "purpose": "cosmeticObservation",
  "requirement": "requiredForVisualEffect",
  "cadenceSteps": 3,
  "maximumRecords": 16,
  "maximumBytesPerResult": 1280,
  "maximumPendingResults": 2
}
```

All eight fields are required when the object is present; additional fields,
duplicate keys, nonboolean `enabled`, and non-unsigned32 numeric fields fail parsing.
The closed purpose names are `authoringPreview`, `diagnosticsCapture`, and
`cosmeticObservation`; requirement names are `optional` and
`requiredForVisualEffect`. Unknown policy names and invalid scalar policy bounds
produce full-pass semantic import findings rather than disabling observations.
There is no gameplay-authoritative purpose.

Import and native preparation call the same pure
`Render::IsValidVfxGpuReadbackPolicy` validator. The schema must be one and cadence
must be positive. Disabled policies require all three storage limits to be zero.
Enabled policies require 1..emitter-capacity records, enough bytes for the normalized
80-byte schema-one record, and 1..64 pending results. Native preparation additionally
uses the actual initial particle count and rejects observed custom-float streams.
These are policy bounds, not proof of hardware support or reserved memory.

`ParticleCookPlan::gpuReadback` preserves the exact validated source policy. A runtime
host must not replace, enable, widen, or silently erase that authored policy. An
explicit `requireCpu` source conflicts with enabled GPU observations and fails import;
actual CPU creation must also reject enabled observation intent when an automatic or
preferred policy resolves to CPU. An optional unsupported native observation route
returns its existing typed unavailable admission; a required route fails. The existing
renderer-owned bounded asynchronous queue, result leases and completion authority
remain unchanged. Result capacity and consumer-held results apply real backpressure.

Rebuild descriptor, cook and runtime consumers together because the C++ carrier and
cook-plan layouts gained the shared policy value. This component supplies authored
opt-in and policy propagation only. It does not qualify D3D12, complete authored GPU
emitter stages, establish new native pixels, or complete HORO-1731.
