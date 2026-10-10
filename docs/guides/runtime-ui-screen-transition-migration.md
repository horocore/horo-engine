# Runtime UI asynchronous screen transitions

`UiScreenTransition.h` belongs to `HoroEngine::RuntimeUi`. The owner composes the
existing `UiRuntimeAssetLoadService`, complete `UiReloadGeneration` validation,
transactional actual `UiScreenStack` and `UiHotReload` publisher. It does not own
jobs, discover services, select a backend or install gameplay/input contexts.

Create it with the host's ownership generation and explicit bounded retention
limits. The borrowed asset loader must outlive the transition owner. All owner
operations run on the serialized Runtime UI thread. The first committed screen
is boot; later commits switch the entire document and actual canvas owners.
There is no automatic fallback asset: failure keeps the current last-good screen;
failure before boot leaves no screen and is surfaced through typed progress/error.
Loading covers and host recovery UI remain the separate RUI-008.7 responsibility.

Call `Begin` outside frame work with the exact canvas reference, target route,
fresh runtime instance and positive timeout. Instance slots strictly increase
within the host-issued ownership generation, including cancelled and failed
admissions. Retain the host's actual `UiElementSlotAllocator` across screens and
reloads; its never-reused namespaces continue to protect old raw element handles.
Use fresh canvas/input owner identities rather than migrating transient handles.
The request pins the current generation and captures its exact route-stack guard.
Preserve the exact `UiScreenTransitionId` returned by `Begin` for every preparation,
cancellation and publication command; a late command cannot target a newer retry.

Advance the existing asset loader outside frame work. `Poll` observes only the
loader's published stage, cooperative cancellation and elapsed host-clock ticks.
Stages are progress facts rather than a fabricated percentage. Clock samples must
be nondecreasing; elapsed subtraction avoids deadline addition overflow. The
deadline includes loading, composition and commit; reaching it exactly expires
the request. Terminal cancellation/timeout cannot be reversed by late completion.
Polling defers provider cancellation/reclamation to load-time collection.

`PrepareAssets` consumes the exact terminal Assets closure and retains original
dependency/provider failures. Borrow `LoadedDocument` to construct detached actual
canvas trees, focus, binding, control, layout and presentation owners using its
exact authored revision. Supply every cooked canvas and an empty actual route
stack with the cooked route catalog. `Prepare` consumes those owners and the
privately retained closure into the existing whole-generation validator. It
activates the selected cooked route inside a private publisher. No candidate
borrow or publication lease escapes. Host-supplied owners must retain their own
provider/module lifetime pins and admit no external input during construction.

`Commit` runs at an ADR-073 structural cutoff and returns an allocation-free typed
outcome. It checks cancellation/deadline, source generation, route guard, held
route transactions and retirement capacity before closing old admission and
swapping whole publishers. Refusal leaves both publishers intact. It performs no
asset pumping, provider I/O, wait, external callback or final generation reclaim.
Commit does not grant input eligibility: the existing exact presentation receipt
contract still gates the new screen's input. Hosts retain whole-generation leases
during render/input extraction and use `IsCurrent` as the publication fence.

Use `Current()->Prepare/Commit` for compatible same-screen reload, preserving the
existing stable-ID reconciliation rules. A reload between transition admission
and publication makes that transition's source stale. Navigation on the original
route stack similarly invalidates its guard; collect/cancel and retry explicitly.
Hosts must not retain the borrowed publisher pointer across transition publication.

`CollectRetired` runs outside frame work. It cancels deferred loader work, destroys
rejected candidates, releases the source pin and collects drained whole publishers.
The underlying asset service continues to own worker cancellation/drain; collection
does not pretend worker completion. Retained render/read leases prevent reclamation
and can backpressure later commits without removing the current screen. Collect
terminal operation storage before starting a new request. Shutdown closes admission
and retires current/candidate publishers without joining provider jobs. Then collect
and drain external leases until `CanReclaim`, before destroying the owner, loader,
asset service and job system in that order. Owner destruction and move assignment
are explicit load-time lifecycle operations.

Collection withdraws all publisher/document borrows and operation admission while
deferred producer/error deleters run. Nested collection and publication are refused;
reentrant shutdown is applied at collection exit. Retiring controls preserve their
immutable asynchronous action/error projections while clearing input, focus and
press state. Their final error allocation/deleter is released only by quiescent
Shutdown or generation reclamation after leases drain, never by screen Commit.

The new public consumer and `HoroRuntimeUiScreenTransitionTests` qualify this
backend-neutral responsibility with the real cooked Assets loader and actual
typed canvas/route owners. Native renderer/IME and aggregate shipping host wiring
remain host composition responsibilities; these tests do not claim those paths.
