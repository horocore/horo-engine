# Cinematic property binding integration

`PropertyEvaluationPlan` remains the low-level bounded sampling contract. Hosts
that need observable playback use `PropertyTrackRuntime` from
`HoroEngine::CinematicRuntime`. It consumes the host's frozen SceneModel
`PropertyBindingRegistry`; there is no cinematic or inspector registry copy.

At sequence activation, compose a mandatory `PropertyDiagnosticSink` borrowing a
host-owned `IPropertyDiagnosticSink` consumer and call
`PropertyTrackRuntime::Create`. Every unresolved binding is inspected and reported
with sequence, track, binding, target, scene and typed outcome before required
binding rejection. Optional failures remain admitted and skip application.
`InspectBindings` also exposes this evidence for authoring validation without
invoking component accessors. Existing low-level Create/Evaluate/Apply callers
retain their result and caller-owned scratch contract. EvaluateAndApply now reports
per-track sampling/target failures and skips only the affected setter, while a
whole-scene fence or insufficient scratch still rejects the boundary before writes.
Direct Evaluate retains its strict all-samples result for callers staging an atomic
batch.

Runtime hosts retain `RuntimePropertyDiagnosticSink` with their DiagnosticsEngine
and EngineDataBus. It publishes `PropertyBindingDiagnosticEvent`, preserves typed
errors in diagnostic history, and emits structured logging fields. Editor hosts
retain `CinematicPropertyProblems` from `HoroEngine::EditorServices`, with their
existing diagnostic and Build Output stores plus canonical sequence source path
and authoring track positions. Problems findings retain a sequence/track path;
Build Output records use the existing source-open routing and navigation policy.
No new widget, panel or renderer dependency is introduced. Inspector recording
uses `InspectorPropertyBindings` to enumerate borrowed descriptors and read typed
values through the same registry's owner getter.

Supply fresh owner-issued component snapshots after lifecycle commit. Each
Evaluate compares the scene generation, binding revision, authored target,
component type, component revision and storage identity before calling a setter.
Scene replacement produces per-track `BindingStale` outcomes. Call Rebind at the
owned lifecycle boundary to invalidate the old plan before resolving replacement
targets. A failed required rebind leaves the controller inactive, never writing
old storage; later explicit rebind can recover. Rebind samples no values and
replays no events. Null component snapshots are reported as missing targets rather
than invalidating unrelated optional bindings.

The host owns authority admission: descriptor setters must honor owner policies.
Do not register physics/controller direct writes or bypass domain request seams.
Registry and immutable curve keys must outlive the controller. Diagnostic adapters
and their services must also outlive all borrowed sinks. The reporting seam uses a typed consumer pointer instead of an erased context and
callback pair: custom consumers implement `IPropertyDiagnosticSink::Publish` and
construct `PropertyDiagnosticSink{&consumer}`. No diagnostic consumer is copied or
allocated by this seam. Virtual dispatch occurs only when reporting evidence; the
successful Evaluate path remains allocation-free. Existing adapter `Sink()` callers
need no change. Controllers are confined
to their scene owner thread; reporting callbacks must not reenter them. Repeated
identical failures are deduplicated until successful recovery or explicit rebind.

Public header ownership now includes `PropertyTrackRuntime.h` in
HoroCinematicRuntime and `CinematicPropertyBindings.h` in HoroEditorServices. The
editor service declares its downward CinematicRuntime dependency. Generated
public-header consumers cover both contracts; HoroCinematicPropertyIntegrationTests
exercises the real runtime notification and editor source-navigation adapters.
