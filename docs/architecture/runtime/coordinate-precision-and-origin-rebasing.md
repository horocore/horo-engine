# Coordinate Precision and Origin Rebasing

## Purpose

This document defines the normative coordinate precision strategy, floating origin rebasing model, GPU shader precision boundaries, physics and subsystem coordinate authority, and lifecycle contracts for large-scale world simulation in Horo Engine.

---

## Core Decisions

- **Hybrid Global Coordinates (`WorldCoordinate64`)**: The authoritative coordinate system for global world space, spatial partitioning, save serialization, and multiplayer replication is a 64-bit composite coordinate (`IntVector3 cellIndex` + `IntVector3 cellOffsetMm`). The stored offset is integer millimeters in the half-open range `[0, cellSizeMm)`, so round-trip conversion to/from world-space `int64_t[3]` millimeters is exact. Conversion to `dvec3` meters is a derived view (exact after 1 mm quantization wherever double ULP is finer than 0.5 mm). fp32 `Vec3` is not a storage field of `WorldCoordinate64`; it is reserved for camera-relative / floating-origin local frames.
- **Floating Origin Rebasing (`CameraRelativeFloat3`)**: Active runtime rendering, physics simulation, audio spatialization, and VFX run in localized 32-bit single-precision floating-point coordinates relative to a dynamic floating origin $C_{\text{origin}}$.
- **Universal GPU 32-bit Shading**: GPU shaders execute exclusively in 32-bit single precision (`fp32`) / half precision (`fp16`). Camera-relative subtraction $(P_{\text{world}} - C_{\text{camera}})$ is evaluated on the CPU or constant-buffer generation during render extraction; shaders receive pre-translated model-view matrices $M_{\text{view\_rel}}$.
- **Zero Velocity Discontinuity**: Origin shifts are discrete coordinate frame translations ($\vec{x}' = \vec{x} - \Delta_{\text{origin}}$), not physical movements over time $\Delta t$. Spatial proxies and transforms are updated without modifying linear/angular velocities, forces, audio pitches (Doppler), or particle lifetimes.
- **Atomic Two-Phase Origin Shift Protocol**: Origin rebasing is coordinated by `OriginRebaseCoordinator` at a declared frame synchronization safe point via an atomic two-phase protocol (`PrepareRebase` -> `CommitRebase`) protected by monotonic `OriginGeneration` fencing.
- **Fallible Typed Error Model**: All coordinate conversions, bounding queries, and rebasing operations conform to [ADR-008](../../adr/008-error-model-exception-boundary-and-registry.md) using `Horo::Result<T, Error>` under the `horo.runtime.precision` error domain.

---

## Mathematical and Coordinate Model

```text
+-------------------------------------------------------------------------+
|                  Global World Space (Canonical Authority)               |
|   WorldCoordinate64 = { IntVector3 cellIndex, IntVector3 cellOffsetMm } |
|   - Spatial partitioning grids & world composition                      |
|   - Save-game serialization & level persistence                         |
|   - Multiplayer network position authority                              |
+-------------------------------------------------------------------------+
                                    |
                    Rebasing Transform (CPU SIMD)
                 P_local = P_world - C_floating_origin
                                    v
+-------------------------------------------------------------------------+
|                 Local Simulation Frame (Rebased Cluster)                |
|   CameraRelativeFloat3 = Vec3 (fp32) relative to C_floating_origin      |
|   - Physics rigid bodies, shapes, constraints, broadphase               |
|   - Audio listener & 3D sound emitters                                  |
|   - VFX particle buffers & simulation graphs                            |
|   - Navigation agent paths & local obstacle avoidance                   |
|   - Camera rigs, orbit controllers, & view matrices                     |
+-------------------------------------------------------------------------+
                                    |
                  Render Extraction (View Matrix Setup)
                     P_cam_rel = P_local - (C_camera - C_floating_origin)
                                    v
+-------------------------------------------------------------------------+
|                     GPU Vertex Pipeline (32-bit FP)                     |
|   v_clip = ProjectionMatrix * ModelView_CameraRelative * v_mesh_local   |
|   - Zero vertex jitter at 10,000 km distances                           |
|   - Zero FP64 ALU penalty on Mobile / WebGL / Console                   |
|   - Maximum FP32 mantissa precision at the camera near plane            |
+-------------------------------------------------------------------------+
```

### 1. Global World Coordinate Types

```cpp
namespace Horo::Math {

/**
 * @brief Discrete 3D integer vector for spatial grid cell indices.
 */
struct IntVector3 {
    int32_t x{0};
    int32_t y{0};
    int32_t z{0};

    [[nodiscard]] constexpr auto operator<=>(const IntVector3 &) const noexcept = default;

    [[nodiscard]] friend constexpr IntVector3 operator+(IntVector3 lhs, IntVector3 rhs) noexcept {
        return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
    }

    [[nodiscard]] friend constexpr IntVector3 operator-(IntVector3 lhs, IntVector3 rhs) noexcept {
        return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
    }
};

/**
 * @brief Authoritative 64-bit composite global world coordinate.
 *
 * Combines an integer cell index with an integer-millimeter offset from the
 * cell minimum corner. Default standard grid cell size is 1024 m = 1,024,000 mm
 * per side. fp32 is not used for stored world authority: at a 1024 m cell edge
 * a 32-bit `Vec3` offset has ULP ≈ 2^-13 m ≈ 0.12 mm, which cannot satisfy a
 * 1 mm exact millimeter round-trip.
 */
struct WorldCoordinate64 {
    static constexpr float   DefaultCellSizeMeters      = 1024.0F;
    static constexpr int32_t DefaultCellSizeMillimeters = 1'024'000;
    static constexpr float   DefaultCellSize            = DefaultCellSizeMeters; // meters; alias for call sites

    IntVector3 cellIndex{0, 0, 0};
    IntVector3 cellOffsetMm{0, 0, 0}; // [0, cellSizeMm) relative to cell minimum corner

    /**
     * @brief True iff cellOffsetMm is in [0, cellSizeMm) on every axis.
     *
     * Integer fields cannot be NaN or Inf. IEEE-754 finiteness is checked only
     * at floating-point conversion boundaries (`FromDoubleMeters`, `Vec3` deltas).
     */
    [[nodiscard]] bool IsNormalized(int32_t cellSizeMm = DefaultCellSizeMillimeters) const noexcept;

    /**
     * @brief Normalizes cellOffsetMm into [0, cellSizeMm) and updates cellIndex.
     */
    [[nodiscard]] Result<WorldCoordinate64> Normalize(int32_t cellSizeMm = DefaultCellSizeMillimeters) const noexcept;

    /**
     * @brief Exact conversion to world-space fixed-point millimeters (1 unit = 1 mm).
     */
    [[nodiscard]] Result<std::array<int64_t, 3>> ToMillimeters(int32_t cellSizeMm = DefaultCellSizeMillimeters) const noexcept;

    /**
     * @brief Constructs WorldCoordinate64 from fixed-point integer millimeters.
     */
    [[nodiscard]] static Result<WorldCoordinate64> FromMillimeters(
        const std::array<int64_t, 3> &mm,
        int32_t cellSizeMm = DefaultCellSizeMillimeters) noexcept;

    /**
     * @brief Derived conversion to double-precision meters. Quantizes to 1 mm.
     * Not a bit-exact inverse of FromDoubleMeters for sub-millimeter inputs.
     */
    [[nodiscard]] Result<std::array<double, 3>> ToDoubleMeters(int32_t cellSizeMm = DefaultCellSizeMillimeters) const noexcept;

    /**
     * @brief Constructs WorldCoordinate64 from double-precision meters.
     * Sub-millimeter fractions round to nearest millimeter (half away from zero
     * is implementation-defined at the 0.5 mm boundary and must be tested).
     */
    [[nodiscard]] static Result<WorldCoordinate64> FromDoubleMeters(
        const std::array<double, 3> &meters,
        int32_t cellSizeMm = DefaultCellSizeMillimeters) noexcept;

    /**
     * @brief Derived fp32 offset in meters for local cluster math. Lossy:
     * worst-case ULP near a 1024 m cell edge is ≈ 0.12 mm.
     */
    [[nodiscard]] Result<Vec3> ToCellOffsetMeters() const noexcept;

    /**
     * @brief Local-cluster delta (lhs - rhs) in meters as fp32 `Vec3`.
     *
     * Not a globally precise subtraction. Rejects non-finite results and deltas
     * whose magnitude exceeds the local physics half-extent R_physics
     * (`PrecisionErrorCode::InvalidRebaseDelta`). Callers that need an exact
     * global delta must use `ToMillimeters()`.
     */
    [[nodiscard]] friend Result<Vec3> SubtractToLocal(
        const WorldCoordinate64 &lhs,
        const WorldCoordinate64 &rhs,
        int32_t cellSizeMm = DefaultCellSizeMillimeters) noexcept;
};

/**
 * @brief Camera-relative or floating-origin relative 32-bit single-precision float coordinate.
 */
using CameraRelativeFloat3 = Vec3;

} // namespace Horo::Math
```

### 2. Numerical Range and Precision Guarantees

| Coordinate Format | Storage Size | Effective Range | Precision at Origin | Precision at $1000\,\text{km}$ |
|---|---|---|---|---|
| `WorldCoordinate64` (Standard Grid) | $12\,\text{bytes} + 12\,\text{bytes} = 24\,\text{bytes}$ | $\pm 2 \times 10^9\,\text{cells} \approx \pm 2 \times 10^{12}\,\text{m}$ | $1.0\,\text{mm}$ constant | $1.0\,\text{mm}$ constant |
| Fixed-point Millimeter (`int64_t[3]`) | $24\,\text{bytes}$ | $\pm 9.22 \times 10^{15}\,\text{m}$ | $1.0\,\text{mm}$ constant | $1.0\,\text{mm}$ constant |
| Double Precision (`dvec3` / `double[3]`) | $24\,\text{bytes}$ | $\pm 1.79 \times 10^{308}\,\text{m}$ | $\approx 2.2 \times 10^{-16}\,\text{m}$ | $\approx 1.2 \times 10^{-7}\,\text{mm}$ |
| Camera-Relative `Vec3` (`fp32`) | $12\,\text{bytes}$ | Local cluster (typically $\le R_{\text{threshold}}$ after rebase; fp32 ULP $\le 1\,\text{mm}$ out to $8192\,\text{m}$) | $\approx 0.00012\,\text{mm}$ (at $1\,\text{m}$) | $\approx 0.061\,\text{mm}$ (within $1\,\text{km}$ of viewer) |

`int64_t` millimeters span $\lfloor \texttt{INT64\_MAX} / 1000 \rfloor \approx \pm 9.22 \times 10^{15}\,\text{m}$. `WorldCoordinate64` is narrower (`cellIndex` is `int32_t`): $2^{31} \times 1024\,\text{m} \approx \pm 2.2 \times 10^{12}\,\text{m}$. Double ULP at $10^{6}\,\text{m}$ is $2^{19-52} = 2^{-33} \approx 1.16 \times 10^{-10}\,\text{m}$ ($\approx 1.2 \times 10^{-7}\,\text{mm}$). Camera-relative fp32 ULP at $1\,\text{m}$ is $2^{-23} \approx 1.19 \times 10^{-7}\,\text{m}$; at $10^{3}\,\text{m}$ it is $2^{-14} \approx 6.1 \times 10^{-5}\,\text{m}$.

---

## Floating Origin Rebasing Architecture

### Origin Frame Identity

`HoroWorldStreaming` owns the backend-neutral identity boundary between canonical
global coordinates and localized simulation values. An immutable `OriginFrame`
binds one stable `OriginFrameId`, exact publication `OriginFrameRevision`,
non-wrapping `OriginGeneration`, and a `WorldCoordinate64` origin. Local values are
an explicit `OriginLocalCoordinate`; they carry frame identity and generation and
cannot be substituted for save, cell, network, or other canonical world authority.

Global-to-local conversion subtracts exact signed integer millimeters before the
bounded result is projected to fp32. The inverse accepts only finite,
millimeter-representable values inside the documented 8192 m local half-extent and
rejects foreign or stale generations. Candidate frame replacement requires exact
revision and generation successors, remains invisible until publication, and
preserves the active frame when validation or storage fails. Cancellation discards
only the unpublished candidate. Replacement and shutdown expire outstanding frame
leases; neither operation mutates any stable global coordinate.

This contract does not choose a trigger, safe point, participant set, or subsystem
adapter. Trigger authority is the separate `OriginShiftPolicy` contract below;
safe-point transactions, participant sets and subsystem adapters remain with the
later `OriginRebaseCoordinator` and integration work described below.

### Origin Shift Trigger and Authority Policy

`HoroWorldStreaming` owns the immutable backend-neutral `OriginShiftPolicy`.
Every publication has a stable `OriginShiftPolicyId`, exact
`OriginShiftPolicyRevision` and stable `OriginFrameId`. Evaluation accepts only
canonical `WorldCoordinate64` focal positions and an exact active-frame binding;
it neither mutates the frame nor invokes a rebase participant.

Each requester has an independent positive millimeter threshold bounded by the
origin frame's 8192 m local half-extent. Distance is radial from the captured
canonical active origin. Equality remains inside the threshold; only a strictly
greater distance produces `RequestShift`. The resulting decision retains the
request identity, active-frame fence, applied threshold and canonical target for
the later safe-point transaction.

Authority is determined entirely by host composition:

| Runtime mode | Gameplay | Editor | Network authority |
|---|---:|---:|---:|
| Standalone gameplay | allowed | rejected | rejected |
| Editor preview | allowed | allowed | rejected |
| Authoritative server | allowed | rejected | rejected |
| Network client | rejected | rejected | allowed |

Thus editor preview can evaluate editor and gameplay focal points without making
editor code a runtime dependency, while a multiplayer client cannot use local
gameplay or editor input to override its server-issued origin authority. Unknown
versions and enum values, stale policy/frame fences, unauthorized requesters and
cancelling/closed lifecycle states return distinct typed errors. Policy
replacement is explicit through revision fencing; cancellation and shutdown close
evaluation without publishing partial state.

### 1. Origin Rebase Coordinator

The `OriginRebaseCoordinator` is owned by `SceneRuntime` and updated during the pre-render frame phase on the host thread that ticks `SceneRuntime` (editor, game, and dedicated-server hosts). It is a runtime type and must not depend on editor types.

```cpp
namespace Horo::Runtime::Precision {

struct OriginRebaseConfig {
    float rebasingThresholdMeters{1000.0F}; // Distance from origin before triggering shift
    float gridCellSizeMeters{1024.0F};      // Spatial partition grid cell size
    bool  snapToGridCells{true};            // If true, origin shifts by discrete cell increments
};

struct OriginRebaseEvent {
    Math::WorldCoordinate64 oldOrigin;
    Math::WorldCoordinate64 newOrigin;
    Math::Vec3              shiftDelta;      // fp32 local-frame view of (newOrigin - oldOrigin); exact mm via old/new Origin
    uint64_t                originGeneration; // Monotonically increasing origin revision
};

enum class RebasePhase : uint8_t {
    Idle,
    Preparing,
    Committing,
    RollingBack
};

class IOriginRebaseParticipant {
public:
    virtual ~IOriginRebaseParticipant() = default;

    [[nodiscard]] virtual std::string_view GetParticipantName() const noexcept = 0;

    /**
     * @brief Phase 1: Validate readiness and pre-allocate migration memory.
     * Must return Error if the subsystem cannot safely shift at this time.
     */
    [[nodiscard]] virtual Result<void> PrepareRebase(const OriginRebaseEvent &event) noexcept = 0;

    /**
     * @brief Phase 2: Apply position translation atomically without velocity alteration.
     */
    virtual void CommitRebase(const OriginRebaseEvent &event) noexcept = 0;

    /**
     * @brief Rollback: Executed if any participant failed during PrepareRebase.
     */
    virtual void RollbackRebase(const OriginRebaseEvent &event) noexcept = 0;
};

class OriginRebaseCoordinator {
public:
    explicit OriginRebaseCoordinator(OriginRebaseConfig config = {}) noexcept;
    ~OriginRebaseCoordinator() noexcept;

    [[nodiscard]] const Math::WorldCoordinate64 &GetActiveOrigin() const noexcept;
    [[nodiscard]] uint64_t GetOriginGeneration() const noexcept;
    [[nodiscard]] bool IsRebasing() const noexcept;

    Result<void> RegisterParticipant(IOriginRebaseParticipant *participant) noexcept;
    Result<void> UnregisterParticipant(IOriginRebaseParticipant *participant) noexcept;

    /**
     * @brief Evaluates camera position and executes atomic two-phase rebase if threshold is breached.
     * Must be called at the declared post-simulation / pre-render synchronization point.
     */
    Result<bool> EvaluateAndRebase(const Math::WorldCoordinate64 &focalWorldPos) noexcept;

    /**
     * @brief Forces an explicit origin shift to a target world coordinate.
     */
    Result<void> ForceRebase(const Math::WorldCoordinate64 &targetOrigin) noexcept;
};

} // namespace Horo::Runtime::Precision
```

### 2. Two-Phase Rebasing Execution Flow

```text
       Main Simulation Frame Safe Point (Pre-Render)
                          │
         Check |P_camera - C_origin| > R_threshold
                          │
                 ┌────────┴────────┐
                 ▼ (Threshold Met) ▼ (Inside Threshold)
          [Begin Rebase]       [Continue Normal Frame]
                 │
  Phase 1: PrepareRebase (All Participants)
    - PhysicsAdapter: Validate broadphase lock state & pre-allocate proxy buffers
    - AudioAdapter: Validate voice locks
    - VfxAdapter: Allocate particle shift buffers
    - SceneRuntime: Validate entity lock state
                 │
       ┌─────────┴─────────┐
       ▼ (All Success)     ▼ (Any Failure)
  Phase 2: CommitRebase    RollbackRebase (Prepared Participants)
    - Apply -Delta_origin  - Restore cached origins
    - Inc OriginGeneration - Emit Diagnostic Error
    - Broadcast Event      - Cancel shift transaction
                 │
       [Resume Frame Pipeline]
```

---

## Subsystem Rebasing Contracts

### 1. Rendering Subsystem

- **GPU Shader Parity**: All vertex and fragment shaders consume standard `float` (`fp32`) and `half` (`fp16`) inputs. No backend requires 64-bit float vertex attributes or double-single software emulation.
- **CPU Camera-Relative Matrices**:
  During Render Extraction, for each render instance:
  $$P_{\text{inst\_cam\_rel}} = P_{\text{inst\_local}} - (C_{\text{camera}} - C_{\text{floating\_origin}})$$
  $$M_{\text{view\_rel}} = V_{\text{rel}} \cdot M_{\text{inst\_rel}}$$
  The constant buffer stores $M_{\text{view\_rel}}$ and $P_{\text{projection}}$.
- **Shadow Map Cascades & Frustum Culling**:
  Shadow cascade splits and frustum culling calculations operate in camera-relative coordinates $P_{\text{cam\_rel}}$, ensuring sub-millimeter shadow map depth precision near the near clipping plane regardless of global distance.

### 2. Physics Subsystem

- **Local Physics World**: The `PhysicsWorld` simulation operates within $[-R_{\text{physics}}, +R_{\text{physics}}]$ centered around the floating origin.
  - $R_{\text{physics}}$ is a `PhysicsWorld` configuration (`localHalfExtentMeters`). It is **not** an alias of $R_{\text{threshold}}$ (`OriginRebaseConfig::rebasingThresholdMeters`, default $1000\,\text{m}$).
  - $R_{\text{threshold}}$ triggers a rebase of the floating origin. $R_{\text{physics}}$ is the half-extent of the local physics AABB after that rebase.
  - Invariant: $\mathrm{ULP}_{\text{fp32}}(R_{\text{physics}}) \le 1\,\text{mm}$, which requires $R_{\text{physics}} < 2^{14}\,\text{m} = 16384\,\text{m}$. Default $R_{\text{physics}} = 8192\,\text{m}$ (eight default $1024\,\text{m}$ cells; ULP $= 2^{13-23} = 2^{-10}\,\text{m} \approx 0.98\,\text{mm}$).
  - Constraint: $R_{\text{physics}} \ge R_{\text{threshold}}$. After a successful rebase every simulated body must satisfy $|\vec{x}_{\text{local}}| \le R_{\text{physics}}$; a body outside this bound is not in the local physics cluster.
- **Coordinate Translation**:
  When `OriginRebaseEvent` commits:
  $$\vec{x}_{\text{body}} \leftarrow \vec{x}_{\text{body}} - \Delta_{\text{origin}}$$
  $$\vec{x}_{\text{collider}} \leftarrow \vec{x}_{\text{collider}} - \Delta_{\text{origin}}$$
  $$\vec{x}_{\text{broadphase\_aabb}} \leftarrow \vec{x}_{\text{broadphase\_aabb}} - \Delta_{\text{origin}}$$
- **Preserved Physical Invariants**:
  - Linear velocity $\vec{v}$ remains unchanged ($\Delta \vec{v} = 0$).
  - Angular velocity $\vec{\omega}$ remains unchanged ($\Delta \vec{\omega} = 0$).
  - Mass, moment of inertia tensor, and center-of-mass offsets remain unchanged.
  - Contact manifolds preserve local contact normals and penetration depths.
  - Sleeping islands remain asleep; no waking impulse is triggered.

### 3. Audio Subsystem

- **Listener & Emitter Positioning**: Listener world positions and 3D spatial emitter coordinates are translated by $-\Delta_{\text{origin}}$.
- **Pitch and Doppler Protection**: The audio spatializer updates positions directly without calculating an artificial position differential $\frac{d\vec{x}}{dt}$ across the rebasing frame, preventing Doppler frequency chirps and volume clicks.

### 4. VFX and Particle Subsystem

- **World-Space Particles**: Live particle position buffers simulated in world space have $-\Delta_{\text{origin}}$ subtracted via SIMD or compute shader dispatch.
- **Local-Space Emitters**: Particles simulated in emitter-local space require zero modification, as their parent entity transform is translated by the Scene Runtime.
- **No Particle Loss**: Origin shifts do not kill, re-seed, or reset particle emitter lifecycles.

### 5. Navigation and AI Subsystem

- **NavMesh Tiles**: Static NavMesh geometry is partitioned per streaming cell. Tile vertices are stored in cell-local coordinates.
- **Dynamic Obstacles & Agents**: Dynamic obstacle cylindrical bounds and agent target corridor waypoints are translated by $-\Delta_{\text{origin}}$.
- **Path Corridors**: Path topological polygon sequence remains intact; agent steering targets are translated without triggering path recalculations.

### 6. Scene Runtime (ECS)

- **Root Transforms**: Root entity transforms store local positions relative to the active floating origin.
- **Global Entity Positioning**: Components requiring absolute world queries resolve `WorldCoordinate64` on demand:
  $$P_{\text{world}} = C_{\text{origin}} + P_{\text{local}}$$

### 7. XR Coordinate and Time Evidence

- XRApi publishes view, local, stage, and world spaces through generation-safe
  `XRCoordinateSpace` values. All translations use metres in Horo's right-handed,
  Y-up, negative-Z-forward convention; native runtime enums and handles remain in
  the concrete XR backend.
- Every space carries the owning session and exact `XRWorldOriginRevision`.
  Replacing a session or committing a world-origin change retires earlier pose
  evidence instead of silently translating it through ambient state.
- `XRPoseSample` stores position, orientation, and velocities as independently
  validity-tagged optional values. Tracking loss removes the affected values;
  identity transforms, zero vectors, and last-known poses are not substitutes for
  missing evidence.
- Runtime sample time, fixed-simulation time, and render-prediction time are
  different strong types. A sample is either committed simulation input or a
  presentation prediction. Presentation predictions cannot rewrite simulation
  state, and coordinate composition requires exact time evidence equality.
- `ComposeXRSpaceTransforms` consumes caller-owned contiguous storage and is
  limited to eight hops. It performs no allocation, blocking I/O, native runtime
  call, or CPU/GPU synchronization on the frame-hot path.

---

## Error Handling and Diagnostics

All fallible precision and rebasing functions return `Horo::Result<T, Error>` under the `horo.runtime.precision` error domain:

```cpp
namespace Horo::Runtime::Precision {

enum class PrecisionErrorCode {
    CoordinateOverflow,          // Coordinate exceeds representable 64-bit world bounds
    NonFiniteCoordinate,         // Input contains NaN or infinite floating-point values
    InvalidRebaseDelta,          // Rebase offset vector is non-finite or exceeds maximum single shift step
    ParticipantRebaseFailed,     // A registered subsystem adapter failed during Prepare or Commit
    UnregisteredParticipant,     // Attempted to interact with an invalid or dead adapter handle
    StaleOriginGeneration,       // Origin generation mismatch during transactional commit
    RebaseDuringSimulationStep,  // Attempted to trigger rebase while physics or render graph was active
    TransactionAborted           // Rebase aborted during preparation phase
};

} // namespace Horo::Runtime::Precision
```

### Invariant Checks and Assertions

1. **Finite floating-point inputs**: IEEE-754 NaN/$\pm\infty$ is checked at conversion boundaries that accept floats (`FromDoubleMeters`, camera-relative `Vec3`, `shiftDelta`) via `Vec3::IsFinite` / `Math::IsFinite`. Failures return `PrecisionErrorCode::NonFiniteCoordinate`. `WorldCoordinate64` stores only integers; it has no `IsFinite()` predicate.
2. **Normalized integer coordinates**: Authority values must satisfy `IsNormalized()` (`cellOffsetMm` in `[0, cellSizeMm)`). Unnormalized offsets are repaired by `Normalize()` or rejected. Cell indices outside $[\text{INT32\_MIN} + 1, \text{INT32\_MAX} - 1]$ return `PrecisionErrorCode::CoordinateOverflow`.
3. **Rebase Safe Point Verification**: `OriginRebaseCoordinator::EvaluateAndRebase` verifies that neither `PhysicsWorld::IsStepping()` nor `RenderFrontend::IsRecording()` is true. Calling rebasing during a step immediately returns `PrecisionErrorCode::RebaseDuringSimulationStep`.

---

## Concurrency, Lifecycle, and Replacement

1. **Thread Role Affinity**:
   - `EvaluateAndRebase` runs strictly on the host thread that ticks `SceneRuntime`. That affinity applies to editor, game, and dedicated-server hosts. It is forbidden on worker, render-owner, transport, and I/O threads. Do not name this an editor `MainEditor` dependency: ADR-010's `MainEditor` wait-policy role is the editor host's main thread only.
   - Subsystem adapters may dispatch parallel workers to update large particle buffers or broadphase structures using `TaskGroup` during `CommitRebase`, joining before `CommitRebase` returns.
2. **Scene Replacement & Reload**:
   - When loading a new scene or unloading the world, `OriginRebaseCoordinator` resets `activeOrigin` to $(0, 0, 0)$ with cell index $(0, 0, 0)$.
   - `originGeneration` increments to invalidate any dangling asynchronous worker references.
3. **Graceful Teardown**:
   - During engine shutdown, all registered participants are unregistered in reverse dependency order.
   - Pending transactions are flushed or cancelled without deadlock.

---

## Verification and Testing Contracts

Automated and integration test suites must cover:

- **Mathematical Conversion Precision**: Exact round-trip conversions between `WorldCoordinate64` and `int64_t[3]` millimeters across extreme ranges ($\pm 10^7\,\text{km}$), including cell-edge values (`cellOffsetMm = cellSizeMm - 1`). `dvec3` round-trip is required only after 1 mm quantization, and only in ranges where double ULP is finer than 0.5 mm. Derived `ToCellOffsetMeters()` must document fp32 ULP growth toward the cell edge (≈ 0.12 mm at 1024 m) and must not be treated as canonical storage.
- **Non-Finite and Boundary Rejection**: `FromDoubleMeters` and other float ingress paths return `NonFiniteCoordinate` for NaN/$\pm\infty$. Integer overflow and unnormalized `cellOffsetMm` return `CoordinateOverflow` or are normalized via `Normalize()` as specified.
- **Two-Phase Transaction Robustness**: Simulating participant failure during `PrepareRebase` and validating clean rollback with zero partial state drift.
- **Velocity and Momentum Preservation in Physics**: Spawning rigid bodies with linear/angular velocity, triggering repeated origin shifts, and verifying that velocities, trajectory curvatures, and sleep states match unshifted baselines within floating-point tolerance ($< 10^{-5}$).
- **Camera-Relative Rendering Accuracy**: Rendering distant meshes ($1000\,\text{km}$ from world origin) using camera-relative vertex shaders and verifying zero vertex jitter or visual artifacting compared to origin-centered renders.
- **Audio Doppler Immunity**: Verifying that listener and emitter rebasing generates zero audio discontinuities or pitch modulations.

---

## Related Documents and ADRs

- [ADR-026: Large-World Precision and Floating Origin Strategy](../../adr/026-large-world-precision-and-floating-origin-strategy.md): Ratified architecture decision and context.
- [World Streaming Architecture](./world-streaming-architecture.md): Spatial partitioning, streaming cells, volumes, and lifecycle.
- [Rendering Architecture](./rendering-architecture.md): Render extraction, camera-relative matrices, and GPU resource ownership.
- [Physics Architecture](./physics-architecture.md): Fixed-step physics, transform authority, and local world simulation.
- [Scene Math](../foundation/scene-math.md): Coordinate conventions, matrices, vectors, and camera projections.
- [Error And Diagnostics](../foundation/error-and-diagnostics.md): Fallible `Result<T, Error>` error domain contracts.
