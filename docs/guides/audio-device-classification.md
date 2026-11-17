# Audio Endpoint Classification Migration

`AudioDeviceClass::Unknown` corrects a false discovery claim: WASAPI, Core Audio
and enumerated SDL3 endpoints previously reported `Physical` without proving
physical routing, while PipeWire reported `Virtual` without proving virtual
routing. Both native physical devices and software or redirected routes can
appear in those catalogs.

`Unknown` is appended with value 3. `Physical` (0), `Virtual` (1), `Headless` (2),
the `std::uint8_t` underlying type, struct layout and snapshot version 1 remain
unchanged. Default-constructed `AudioDiscoveredDevice` now uses `Unknown`.
Validation accepts it for WASAPI, Core Audio, PipeWire and SDL3, while Null still
requires `Headless`. Selection and generation checks do not depend on physical
classification. Older validators reject an unknown enum value; callers exchanging
these C++ contracts must rebuild against the matching AudioApi implementation.
This change introduces no serialized project-data migration.

The four discovery adapters now use `Unknown` for entries whose native evidence
does not determine the route class. SDL3's synthetic system-default endpoint
remains `Virtual`, and Null's deterministic endpoint remains `Headless`. Native
evidence may support more precise classification in a later implementation;
absence of that evidence must not become a guessed physical or virtual claim.

Affected consumers are the native/SDL3 discovery adapters, snapshot validation
and any host diagnostics or qualification tools interpreting `deviceClass`.
Update exhaustive switches to handle `Unknown`, keep it selectable and require
independent route evidence before claiming physical-device qualification. Callers
constructing known physical, virtual or headless fixtures should set that class
explicitly. Header ownership stays with `HoroAudioApi`; no target dependency or
public-header ownership changes are required.

`HoroAudioApiTests` adds unknown-class discovery/default/explicit-selection and
Null-rejection regressions. `HoroAudioApiPublicHeaderConsumer` adds numeric-value
and underlying-type assertions. These sources have not yet been compiled or run
for this delivery. All affected native, SDL3 and public-header consumers still
require their platform builds; this migration records no hardware pass.
