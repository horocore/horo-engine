# Generic material binding migration

HORO-378 adds a renderer-owned reflected binding boundary. All three public
headers (`MaterialBinding.h`, `MaterialBindingBackend.h`,
`MaterialBindingErrors.h`) belong to `HoroEngine::RenderApi`; consumers receive
only the owning target's staged header surface. `RenderBackend.h` includes only
the small adapter interface, keeping packed PBR/reflection/vector/variant inputs
outside its direct dependency surface.

Existing scene semantic tables, `PrepareStandardPbrMaterial`, pass planning,
shader reflection and pipeline preparation contracts remain their sources of
truth. No caller changes are mandatory. A RenderFrontend composition creates a
`MaterialBindingTable` with its selected `IRenderBackend` and finite limits.
After cooked variant and product-profile admission, normalize the exact final
reflection, provide every active resource array element and owned packed bytes,
and publish at a render-capable preparation safe point. Backend adapters must
resolve exact pipeline/resource generations and usage/range compatibility
before creating a native table; the returned lease owns the corresponding
registry pins. Concrete native realization is delivered by each backend ticket,
including HORO-319 for Metal. The default boundary fails with typed Unsupported;
this change does not claim any installed backend now renders these bindings.

Frame consumers acquire an exact published ID. They borrow immutable descriptor
storage and the adapter lease for their submission lifetime. An adapter retains
native command-use references until native completion even if preparation
leases are released. To replace a binding, publish the new generation, switch
future extraction to its ID, then release the old table reference. Consumer-held
old generations still consume the same finite budgets; a full table returns
CapacityExceeded before adapter work. A failed candidate leaves the old entry
available.

Required materials cannot request a fallback. An optional fallback names an
already resident entry with identical interface and target layout. Only exact
adapter Unsupported permits it; the returned selection records the original
error and fallback flag. Stale generations, malformed input and other backend
errors remain failures, with no hidden shader compilation or variant change.

All methods and final acquired-lease destruction run on the creating render
thread. Stop producers before Shutdown; it closes discovery and releases table
references, while already acquired leases remain valid. Retire/abandon native
submissions and drain those leases before destroying the backend or registry.
No destructor waits for normal-frame GPU completion. Cancellation belongs to
host admission before this synchronous preparation call; no worker captures,
background jobs or callbacks are retained.

`HoroMaterialBindingTests` covers validation, publication, failure preservation,
fallback decisions, immutable leases, finite retained budgets and shutdown.
`HoroMaterialBindingPublicHeaderConsumer` builds through RenderApi only. Both
are included in the reduced Windows CI build/discovery closure; Linux/macOS
full suites and hosted quality checks must qualify the actual C++ sources.
