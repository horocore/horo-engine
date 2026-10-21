# D3D12 resource service

HORO-332 implements the resource responsibility independently of D3D12 command
recording and presentation. `D3D12Initialization` owns the native device, direct
queue and `ID3D12Resources` service. After successful startup its `Resources()`
method lends the service on the render owner thread. It implements the existing
`IRenderResourceBackend` contract, so allocation/registry callers retain one
canonical descriptor, placement and resource-generation authority. This does not
advertise a complete interactive D3D12 renderer or register a backend implicitly.

## Ownership and preparation

Resource allocation and transfer preparation are load/preparation operations,
serialized outside frame recording. No native types enter `include/Horo/`.
The native constructor seam is private to `HoroRenderD3D12`; it retains the exact
device and verifies that the supplied direct queue belongs to it. Initialization
builds bounded descriptor heaps (shader-visible CBV/SRV/UAV and sampler heaps,
CPU-only RTV/DSV heaps) and a fence after host/runtime admission.
Failure or cancellation before readiness rolls back the entire startup session.

Call `QueryBufferMemoryCost` or `QueryTextureMemoryCost`, reserve that exact plan
in the frontend memory ledger, then pass its `RenderMemoryPlacement` to creation.
Compatible resources use explicit shared heaps and placed resources. Size and
alignment come from the selected device. The frontend chooses and charges the
whole backing capacity; the native service never rounds it up or grants slack.
A placement must preserve class, compatibility, provenance, payload and required
bytes, fit its admitted capacity, and avoid every live or retiring native range.
Reuse requires the exact pool, scope, capacity, alignment and tier-1-safe native
heap class. Buffer, upload, ordinary texture, attachment and MSAA attachment
compatibilities stay distinct. There is no independent native pool-size policy.
`Snapshot` counts each heap's whole capacity once until its final native allocation
retires; freeing one suballocation does not pretend the remaining heap vanished.
Native allocation failure retains the original HRESULT.

Buffers support device-local storage and CPU-visible upload storage. GPU-written
host-visible buffers are rejected. Uniform buffers require native 256-byte size
alignment and at most 64 KiB; raw storage buffers require four-byte elements.
Texture queries check native format/usage/sample support and field-width limits.
Depth and linear/sRGB families use typeless storage with explicit typed views.
Packed depth/stencil attachment views make omitted write planes read-only.
Views create SRV, UAV, RTV and DSV descriptors as applicable; sampler creation
uses the canonical immutable filtering/comparison policy. Composite mesh/target
records retain their exact buffer/view dependencies.

Array storage requires array shader views, and cube views with nonzero base faces
require CubeArray. Storage/attachment views expose one mip; sampled-only views
may expose multiple mips. These unsupported interpretations return typed errors.
Depth/MSAA transfer payloads are explicitly unsupported rather than silently
converted. All other uncompressed color transfers support 1D, 2D, arrays and 3D,
with arbitrary row widths and base-level-only initialization matching the public
creation contract. Upper mip contents remain undefined until separately produced.

## Transfers, accounting and retirement

Initial uploads are copied synchronously into bounded owned staging, submitted on
the initialized direct queue, and ordered ahead of subsequent use on that queue.
CPU-visible initial buffer bytes map directly. Default resource transfers use
first-access alias activation for placed memory and explicit COMMON to
COPY_SOURCE/COPY_DEST to COMMON barriers. Future D3D12 command
recording must activate unused placed resources and use this same state boundary; another queue cannot consume these
resources without its own explicit synchronization handoff.

`Upload`/`Readback` operate on private backend instances after canonical frontend
identity resolution. Live uploads require CopyDestination usage; immutable
initial bytes use a separate pre-publication write path. CPU-visible upload
buffers cannot be mutated after publication through the live upload API. They are resource-service operations, not a second public
resource registry or a replacement for `RenderUploadQueue`/`RenderReadbackQueue`.
The existing frontend transfer queues remain lifecycle/accounting models; host
adapters can use their payload/completion contracts with this native executor.
No new frontend transfer submission API is introduced here.

Every transfer has a unique local identity, dedicated upload/readback backing,
command allocator/list and fence point. Native row pitches are expanded on upload
and stripped into owned bytes on readback. Finite host configuration bounds
requests and *native aligned backing*, including padding; forgotten CPU readbacks
retain their charge and cause backpressure. Cancellation stops publication and
retains submitted work until completion. A failed queue signal retains all
references and is retried by `Poll`; no failed Execute/Signal path frees work that
may still be executing. Device removal remains a distinct error.

The frontend's dependency and submission pins govern `Destroy*`. Internally,
transfer and composite references defer actual native/descriptor reclamation.
Identifiers never recycle; exhausted counters return capacity errors. Descriptor
slots recycle only after the last dependency/transfer reference disappears.
`Snapshot` separates resource/transfer backing, retiring backing, staging and the
estimated fixed descriptor infrastructure charge. The configured four descriptor
heaps have a conservative 64-KiB estimate each (at most 2048 descriptors per heap);
sampler plans conservatively charge 64 KiB per sampler as estimated requirements.
These conservative policy estimates are not claims of exact driver allocation.
DXGI local/nonlocal process usage and budgets are fresh observations through
`QueryResidency`; they never increase the host allowance and may overlap on UMA.
There is no eviction of live resources or hidden fallback under pressure.

Before teardown, finish external submissions and frontend pins, call
`StopAdmission`, and poll `PollDrain`. This abandons CPU results and drains copies
without a CPU/GPU wait. `D3D12Initialization::Shutdown` performs the same protocol,
returning `render.d3d12.drain_pending` until release is safe. Destruction requires
a drained service unless the native device is actually removed. Migration from
the startup-only session therefore requires hosts using resource work to retry
pending shutdown rather than destroying an active session. Existing startup-only
callers have no in-flight resource work and retain synchronous shutdown behavior.
This private service does not modify HORO-326's public submission/drain authority.

## Qualification

`HoroD3D12ResourceTests` runs portable bound validation on every platform and
explicit WARP native tests on Windows x64. WARP is test-selected, with no production
software fallback. Native resource tests construct an actual WARP device/direct
queue independently of Windows 11 workstation/Agility startup admission, allowing
Windows Server CI to qualify resource operations without claiming host startup
support. Bounded test polling is permitted solely for test completion.

Coverage includes buffer and pitched texture byte round trips, arrays/volumes,
linear/sRGB view families, native descriptor diagnostics when installed,
aligned shared placement/overlap rejection and whole-heap accounting, descriptor exhaustion and reuse,
composite dependency retention, sampled depth, samplers/raw/constant buffers,
resource/transfer backpressure, cancellation, owner-thread rejection and drain.
`HoroD3D12InitializationTests` covers resource-stage failure/cancellation rollback.
Both targets consume only the three staged startup/resource test headers through
the non-installed `HoroRenderD3D12TestingInternal` interface; neither publishes or
receives a repository-wide source include root. Production internals remain
private. Build both targets and run their tests on Windows; Linux/macOS cover
portable omission and startup policy only. Hosted build/test results are distinct from
local source checks and from physical-device driver qualification.

## Canonical frontend retirement handoff

The native consumer uses HORO-326's move-only `RenderResourceNativeLifetime` on
the five core creation methods: buffers, meshes, textures, views and targets.
The exact owned bundle moves into the native resource record before heap
allocation or enqueue. Its member precedes the resource/heap COM members, so
reverse destruction drops native backing before marking the lease released.
Descriptor slots are returned before that record is destroyed. Creation rollback
also retires unpublished records during metadata-exception unwind. Native copy
references retain the same record and lease after cancellation or failed
creation following accepted Execute. Release only marks owner-thread evidence;
it never reenters registry or ledger callbacks.

The frontend retains stable lease addresses, resource generations and their one
existing memory charge until its drain consumes terminal native evidence.
Successful creation converts that same reservation to an allocation. Failed
creation after accepted native work retains the reservation until native
retirement. A void Destroy request alone is not a memory acknowledgement.
Synchronous sampler preparation retains its existing synchronous contract and
never enqueues private copy work.

Each admitted native resource context acquires its incarnation once from the
shared process-wide `AcquireRenderDeviceIncarnation` producer during actual
initialization. Exhaustion propagates its typed failure before readiness; no
backend-local device counter or metadata-constructor allocation exists. Resource
snapshots project that same identity, and transfer tokens bind it to their local
value. Zero, foreign and stale tokens are rejected before native completion
counter polling. A future D3D12 capability snapshot must project this context
identity rather than create a second one.

WARP regressions cover one claim/release across all five creation methods,
invalid-placement rollback, cancellation/shutdown, copy/readback retention and
device removal immediately after actual Execute and between readback polling
and its completion query. These empty-by-default target-private hooks qualify
real device-removal boundaries; production passes no hooks.
The same-value-one regression creates two real contexts and rejects the stale
incarnation. Portable session tests separately preserve pending/failed drain
state without releasing the native owner early. The descriptor diagnostic test
explicitly skips when Windows diagnostic layers are unavailable; byte-transfer
and lifetime tests run independently of that optional diagnostic evidence.

## Native API references

- [D3D12 copy footprints](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-getcopyablefootprints)
- [Texture upload/readback layout](https://learn.microsoft.com/en-us/windows/win32/direct3d12/upload-and-readback-of-texture-data)
- [Explicit resource barriers](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12)
- [Residency and DXGI observations](https://learn.microsoft.com/en-us/windows/win32/direct3d12/residency)
- [Explicit test device removal](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-removedevice)

- [Placed-memory alias activation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/memory-aliasing-and-data-inheritance)
