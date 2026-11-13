#pragma once

/** @file PhysicsStateSnapshot.h
 * @brief Physics-owned versioned continuation values. Native recorder bytes are
 * never this format.
 */

#include "Horo/Foundation/Sha256.h"
#include "Horo/Physics/PhysicsContinuousCollision.h"
#include "Horo/Physics/PhysicsEvents.h"
#include "Horo/Physics/PhysicsIdentity.h"
#include "Horo/Physics/PhysicsStateConstructionManifest.h"
#include "Horo/Physics/PhysicsStateConstructionTypes.h"
#include "Horo/Physics/PhysicsWorldSettings.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Physics {
    namespace Detail {
        struct PhysicsWorldSnapshotState;
        struct PhysicsStateSnapshotSlot;
    }  // namespace Detail

    /** @brief Exact supported typed continuation schema; unknown required data
     * fails closed. */
    inline constexpr std::uint32_t PhysicsStateSnapshotVersion = 2;
    /** @brief Exact adapter grammar for pinned Jolt 5.6.0 float CPU state. */
    inline constexpr std::uint32_t PhysicsStateSolverSchemaVersion = 1;
    /** @brief Conservative schema ceilings from the pinned native shared-cache
     * maximum, not its bucket counts. */
    inline constexpr std::uint32_t MaximumPhysicsStateCachePairs =
        static_cast<std::uint32_t>((std::uint64_t{MaximumPhysicsResourceRecords} * 204) / 40);
    inline constexpr std::uint32_t MaximumPhysicsStateCacheManifolds =
        static_cast<std::uint32_t>((std::uint64_t{MaximumPhysicsResourceRecords} * 204) / 80);

    /** @brief Transient complete checkpoint binding; the receiving Physics owner
     * resolves the actual current handle.
     * @details This input is not serialized. One nonzero stable ID per resident
     * body is required, including query fixtures.
     */
    struct PhysicsStateBodyBinding final {
        PhysicsStateObjectId id{};
        BodyHandle body;
    };

    /** @brief Transient complete joint binding, never a native index or durable
     * Physics handle. */
    struct PhysicsStateConstraintBinding final {
        PhysicsStateObjectId id{};
        ConstraintHandle constraint;
    };

    /** @brief Transient complete immutable shape binding, including compound
     * intermediates and query-fixture shapes. */
    struct PhysicsStateShapeBinding final {
        PhysicsStateObjectId id{};
        ShapeHandle shape;
    };

    /** @brief Exact compatibility tuple, not a determinism qualification or
     * authority to resolve new content.
     * @details The process composition supplies verified binary/build and platform
     * digests before state capability admission. Physics supplies actual settings
     * and retained topology/filter/surface digests. No field may be zero. Save,
     * Replay, Rollback and Networking store this SAME schema; adapters do not
     * serialize native recorder bytes.
     */
    struct PhysicsStateCompatibility final {
        std::uint32_t formatVersion{PhysicsStateSnapshotVersion};
        std::uint32_t solverSchemaVersion{PhysicsStateSolverSchemaVersion};
        std::uint32_t settingsSchemaVersion{PhysicsWorldSettingsSchemaVersion};
        Sha256Digest build;
        Sha256Digest platform;
        Sha256Digest solver;
        PhysicsWorldSettingsIdentity settings;
        Sha256Digest content;
        Sha256Digest topology;
        Sha256Digest filters;
        Sha256Digest surfaces;
        PhysicsStateFpControl floatingPoint;
        auto operator<=>(const PhysicsStateCompatibility &) const noexcept = default;
    };

    /** @brief One complete native sleep-test sphere, in local world meters. */
    struct PhysicsStateSleepSphere final {
        std::array<float, 3> center{};
        float radius{};
        auto operator<=>(const PhysicsStateSleepSphere &) const noexcept = default;
    };

    /** @brief Required moving-body continuation; static bodies omit this section
     * explicitly. */
    struct PhysicsStateMotion final {
        std::array<float, 3> linearVelocity{}, angularVelocity{}, force{}, torque{};
        std::array<PhysicsStateSleepSphere, 3> sleepSpheres{};
        float sleepTimer{};
        bool allowSleeping{};
        auto operator<=>(const PhysicsStateMotion &) const noexcept = default;
    };

    /** @brief Complete mutable rigid-body solver state, sorted by stable id.
     * @details Position is center of mass, not the authored body origin. Rotation
     * is x/y/z/w. hasMotion exactly matches retained topology; absent motion fields
     * must be zero. Immutable mass, solver-active dynamic inertia, shape,
     * friction and motion policy are topology compatibility inputs. Inactive
     * reserved inertia is deliberately omitted: the supported owner transition
     * reapplies admitted mass/inertia before a static/kinematic body can take
     * its first dynamic solver step; custom activation consumers are unsupported.
     */
    struct PhysicsStateBody final {
        PhysicsStateObjectId id{};
        std::array<float, 3> centerOfMass{};
        std::array<float, 4> rotation{0, 0, 0, 1};
        PhysicsStateMotion motion;
        bool hasMotion{}, active{};
        bool hasPolicyIntent{};
        std::array<float, 3> lastLinearIntent{}, lastAngularIntent{};
        auto operator<=>(const PhysicsStateBody &) const noexcept = default;
    };

    /** @brief Closed retained native joint families; drive support is not implied.
     */
    enum class PhysicsStateConstraintKind : std::uint8_t {
        Fixed,
        Distance,
        Hinge,
        Slider
    };

    /** @brief Required mutable joint state with explicit family-specific meanings.
     * @details Fixed: rotationLambda3, pointLambda3. Distance: axisLambda,
     * previousNormal3. Hinge: motorLambda, transverseLambda2, pointLambda3,
     * limitLambda and motor targets. Slider: motorLambda, transverseLambda2,
     * rotationLambda3, limitLambda and motor targets. Unused fields must be exactly
     * zero; enabled and motorState are always explicit. Motor states are 0..3.
     */
    struct PhysicsStateConstraint final {
        PhysicsStateObjectId id{};
        PhysicsStateConstraintKind kind{};
        bool enabled{};
        std::array<float, 3> rotationLambda{}, pointLambda{}, previousNormal{};
        std::array<float, 2> transverseLambda{};
        float axisLambda{}, motorLambda{}, limitLambda{}, targetVelocity{}, targetPosition{};
        std::uint8_t motorState{};
        auto operator<=>(const PhysicsStateConstraint &) const noexcept = default;
    };

    /** @brief A solver warm-start contact point, never a truncated gameplay contact
     * summary. */
    struct PhysicsStateContactPoint final {
        std::array<float, 3> position1{}, position2{};
        float normalLambda{};
        auto operator<=>(const PhysicsStateContactPoint &) const noexcept = default;
    };

    /** @brief Pinned-shape subshape codes; their interpretation requires exact
     * solver/topology compatibility.
     * @details Native body IDs never appear here. Body IDs are stable checkpoint
     * identities. Keys are strictly increasing; endpoint orientation is retained,
     * because reversing it would alter normal/lambda semantics.
     */
    struct PhysicsStateManifoldKey final {
        PhysicsStateObjectId first{}, second{};
        std::uint32_t firstSubshape{}, secondSubshape{};
        auto operator<=>(const PhysicsStateManifoldKey &) const noexcept = default;
    };

    /** @brief One complete solver manifold; fixed four-point bound matches the
     * pinned adapter. */
    struct PhysicsStateManifold final {
        PhysicsStateManifoldKey key;
        std::array<float, 3> normal{};
        std::array<float, 2> frictionLambda{};
        float angularFrictionLambda{};
        std::uint32_t pointCount{};
        std::array<PhysicsStateContactPoint, 4> points{};
        auto operator<=>(const PhysicsStateManifold &) const noexcept = default;
    };

    /** @brief Broadphase contact-cache pair; contiguous manifold range belongs to
     * this exact pair.
     * @details relativeRotation is Jolt's compressed x/y/z quaternion with
     * nonnegative reconstructed w.
     */
    struct PhysicsStateBodyPair final {
        PhysicsStateObjectId first{}, second{};
        std::array<float, 3> relativePosition{}, relativeRotation{};
        std::uint32_t manifoldBegin{}, manifoldCount{};
        auto operator<=>(const PhysicsStateBodyPair &) const noexcept = default;
    };

    /** @brief Independent caller-admitted storage ceilings; checked before
     * allocation or native reconstruction. */
    struct PhysicsStateLimits final {
        std::uint32_t maximumBodies{}, maximumConstraints{}, maximumPairs{}, maximumManifolds{}, maximumCcdManifolds{};
        std::size_t maximumBytes{};
    };

    /** @brief Borrowed complete typed solver continuation; every span remains valid
     * through the synchronous call.
     * @details Flat keyed arrays are strictly ordered. activeBodyOrder is an
     * ordered semantic permutation of active bodies, not an enumeration order to
     * sort away. All required spans, including empty ones, exist. Storage
     * preparation belongs to Physics; no reader retains these spans. Horo event
     * continuation is a separate required section in the enclosing snapshot, not
     * inferred from solver manifolds or regenerated enter events.
     */
    struct PhysicsStateSolverView final {
        float previousDelta{};
        std::array<float, 3> gravity{};
        std::span<const PhysicsStateBody> bodies;
        std::span<const PhysicsStateObjectId> activeBodyOrder;
        std::span<const PhysicsStateConstraint> constraints;
        std::span<const PhysicsStateBodyPair> pairs;
        std::span<const PhysicsStateManifold> manifolds;
        std::span<const PhysicsStateManifoldKey> ccdManifolds;
    };

    /** @brief Durable event endpoint identity, rebound to actual current handles
     * under exact immutable compatibility.
     * @details filterSchemaGeneration and PhysicsWorldId are reissued by the
     * receiving owner, never restored as authority.
     */
    struct PhysicsStateEventEndpoint final {
        PhysicsStateObjectId body{}, shape{};
        std::optional<PhysicsShapeSubresourceId> subshape;
        CollisionLayerId layer;
        CollisionProfileId profile;
        std::uint64_t collider{};
        auto operator<=>(const PhysicsStateEventEndpoint &) const noexcept = default;
    };

    /** @brief Complete Horo contact/trigger lifecycle state; solver manifolds
     * cannot regenerate this without duplicate enter events. */
    struct PhysicsStateEventPair final {
        PhysicsStateEventEndpoint first, second;
        std::optional<PhysicsEventMaterial> firstMaterial, secondMaterial;
        std::optional<PhysicsStateSurfaceEvidence> firstSurface, secondSurface;
        PhysicsContactSummary contact;
        bool sensor{};
        auto operator<=>(const PhysicsStateEventPair &) const noexcept = default;
    };

    /** @brief One complete already-published event; restoring it is not a new
     * gameplay side-effect delivery. */
    struct PhysicsStatePublishedEvent final {
        std::uint64_t tick{};
        std::uint32_t ordinal{}; /**< Exact committed lifecycle emission order; never
                                    regenerated from native callback arrival. */
        PhysicsEventKind kind{};
        PhysicsStateEventPair pair;
        auto operator<=>(const PhysicsStatePublishedEvent &) const noexcept = default;
    };

    /** @brief Required Horo continuation section, separate from native solver state
     * and external side-effect history. */
    struct PhysicsStateWorldView final {
        std::uint64_t completedTick{};
        PhysicsContinuousCollisionPolicy continuousCollision;
        std::uint32_t appliedCommands{}; /**< Historical command count at the restored
                                            completed tick, not new execution. */
        std::uint64_t droppedEventCount{};
        std::span<const PhysicsStateEventPair> previousPairs;
        std::span<const PhysicsStatePublishedEvent> publishedEvents;
    };

    /** @brief Explicit artifact identities supplied by real process composition,
     * never generated from a label.
     * @details Hashes identify actual executable/Physics code artifacts. The host
     * retains their release/module leases. Binding grants no ADR088 determinism
     * qualification or cross-build migration.
     */
    enum class PhysicsStateCodeLifetime : std::uint8_t {
        Unspecified,
        ProcessStatic,
        ModuleLease /**< Reserved/unavailable until a genuine loader
                       execution/retirement fence is integrated. */
    };

    struct PhysicsStateBuildBinding final {
        Sha256Digest executableArtifact;
        Sha256Digest physicsArtifact;
        Sha256Digest platformAbi;
        PhysicsStateCodeLifetime lifetime{PhysicsStateCodeLifetime::Unspecified};
        std::shared_ptr<const void> codeLease;
    };

    /** @brief Explicit runtime-wide aggregate admission, shared by active worlds
     * and captures retained after their shutdown.
     * @details Per-world configuration ceilings do not replace this aggregate
     * reservation. Ownership records are cold reserved; old reservations retire
     * only on the owner thread after actual consumer references are gone.
     */
    struct PhysicsStateRuntimeBudget final {
        std::uint32_t maximumCaptureOwners{};
        std::size_t maximumRetainedBytes{};
        PhysicsStateConstructionLimits construction;
        PhysicsStateConstructionTracking constructionTracking{PhysicsStateConstructionTracking::Unspecified};
        /**< Explicit finite transitive recipe tracking prepared before any World/resources are created.
             No automatic growth/default capacity; its actual payload is charged once to this Runtime budget. */
    };

    enum class PhysicsRuntimeRetirementPhase : std::uint8_t {
        WaitingWorlds,
        WaitingStateLeases,
        WaitingNativeCandidates,
        Drained
    };

    /** @brief Actual bounded owner retirement observation, not inferred from a
     * stopped flag or caller release. */
    struct PhysicsRuntimeRetirementProgress final {
        PhysicsRuntimeRetirementPhase phase{PhysicsRuntimeRetirementPhase::WaitingWorlds};
        std::uint32_t pendingWorlds{}, stateLeaseRecords{}, nativeCandidates{}, retiredRecords{};
    };

    /** @brief Move-only same-runtime retirement owner; no process-global retired
     * owner registry exists.
     * @details Obtain before dropping the public runtime when state captures may
     * outlive worlds. Close worlds and settle actual Save/network workers, then
     * retain this handle until owner-thread Collect reports Drained. Dropping the
     * final owner with pending native state leases is a lifecycle contract
     * violation, never implicit successful cleanup. Immutable capture release on a
     * worker only drops references; it cannot finalize Jolt globals.
     */
    class PhysicsRuntimeRetirement final {
    public:
        PhysicsRuntimeRetirement() noexcept = default;
        ~PhysicsRuntimeRetirement();
        PhysicsRuntimeRetirement(PhysicsRuntimeRetirement &&) noexcept = default;
        PhysicsRuntimeRetirement &operator=(PhysicsRuntimeRetirement &&) noexcept;
        PhysicsRuntimeRetirement(const PhysicsRuntimeRetirement &) = delete;
        PhysicsRuntimeRetirement &operator=(const PhysicsRuntimeRetirement &) = delete;
        /** @brief Examines at most maximumRecords with a fair bounded cursor; no
         * blocking wait or global reconstruction. */
        [[nodiscard]] Result<PhysicsRuntimeRetirementProgress> Collect(std::uint32_t maximumRecords);

    private:
        friend class PhysicsRuntime;

        explicit PhysicsRuntimeRetirement(std::shared_ptr<void> owner) noexcept : owner_(std::move(owner)) {}

        std::shared_ptr<void> owner_;
    };

    /** @brief Cold capture admission under one exact resident world/content
     * generation.
     * @details Slots retain immutable values until all consumers release them. Full
     * slots return CapacityExceeded before capture; no snapshot is overwritten.
     * maximumRetainedBytes charges actual vector capacities and fixed owner payload
     * once, excluding allocator/control-block/driver overhead. Native candidate
     * memory is separately bounded to one complete additional solver with the
     * world's existing native capacities/scratch reservations.
     */
    struct PhysicsStateCaptureConfiguration final {
        std::uint32_t snapshotSlots{2};
        std::size_t maximumRetainedBytes{};
        Sha256Digest content;
        std::span<const PhysicsStateBodyBinding> bodies;
        std::span<const PhysicsStateShapeBinding> shapes;
        std::span<const PhysicsStateConstraintBinding> constraints;
        std::shared_ptr<const void> contentLease;
        std::span<const PhysicsStatePersistentOwner> owners; /**< Optional generic profile; Scene Save supplies exact complete full keys. */
    };

    /** @brief Immutable complete capture lease, shared by
     * Save/Replay/Rollback/Networking without alternate state formats. */
    class PhysicsStateSnapshot final {
        struct Impl;

    public:
        ~PhysicsStateSnapshot();
        PhysicsStateSnapshot(const PhysicsStateSnapshot &) = delete;
        PhysicsStateSnapshot &operator=(const PhysicsStateSnapshot &) = delete;
        [[nodiscard]] const PhysicsStateCompatibility &Compatibility() const noexcept;
        [[nodiscard]] PhysicsStateSolverView Solver() const noexcept;
        [[nodiscard]] PhysicsStateWorldView World() const noexcept;
        /** @brief Complete canonical bounded wire shared verbatim by
         * Save/Replay/Networking; valid while this lease lives. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;
        /** @brief Required immutable constructor subsection of Bytes(), using the SAME canonical codec.
         * Borrowed from this opaque lease. It grants no new content, native resource or Scene publication authority. */
        [[nodiscard]] std::span<const std::byte> ConstructionBytes() const noexcept;
        /** @brief Actual slot payload capacity; shared immutable content/code leases
         * are separately charged. */
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;

    private:
        friend struct Detail::PhysicsWorldSnapshotState;
        friend class PhysicsWorld;
        friend class PhysicsStateRestoreCandidate;
        explicit PhysicsStateSnapshot(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };

    /** @brief Opaque immutable capture value; copies retain a reader and no
     * internal pooled shared_ptr is exposed.
     * @details Values may be copied/read/released on workers. Views remain valid
     * while a lease lives. The owner seals an unleased slot before rewriting it; a
     * weak_ptr cannot resurrect a pooled capture. Default/moved-from values are
     * invalid. Successful publication/copy/move/release never allocates. Native
     * resource retirement remains owner-thread work in PhysicsRuntime's actual
     * inventory.
     */
    class PhysicsStateSnapshotLease final {
    public:
        PhysicsStateSnapshotLease() noexcept = default;
        ~PhysicsStateSnapshotLease();
        PhysicsStateSnapshotLease(const PhysicsStateSnapshotLease &other) noexcept;
        PhysicsStateSnapshotLease &operator=(const PhysicsStateSnapshotLease &other) noexcept;
        PhysicsStateSnapshotLease(PhysicsStateSnapshotLease &&) noexcept = default;
        PhysicsStateSnapshotLease &operator=(PhysicsStateSnapshotLease &&other) noexcept;

        [[nodiscard]] bool IsValid() const noexcept {
            return slot_ != nullptr;
        }

        /** @brief Returns immutable values or null for an invalid lease; borrow is
         * bounded by this lease lifetime. */
        [[nodiscard]] const PhysicsStateSnapshot *Get() const noexcept;

        [[nodiscard]] const PhysicsStateSnapshot *operator->() const noexcept {
            return Get();
        }

    private:
        friend struct Detail::PhysicsWorldSnapshotState;

        explicit PhysicsStateSnapshotLease(std::shared_ptr<Detail::PhysicsStateSnapshotSlot> slot) noexcept : slot_(std::move(slot)) {}

        void Release() noexcept;
        std::shared_ptr<Detail::PhysicsStateSnapshotSlot> slot_;
    };

    /** @brief Owner-bound unpublished restore; destruction aborts native state
     * without changing the live world.
     * @details ValidatePublication rechecks world, joined phase,
     * build/content/topology, cancellation and publication revision. Publish runs
     * only at the same drained owner fence after validation, without intervening
     * owner work. It preserves world/body/constraint handle generations, revokes
     * old query/event access, and publishes one complete restored tick with a NEW
     * monotonic publication revision. It never re-delivers external side effects
     * automatically.
     */
    class PhysicsStateRestoreCandidate final {
        struct Impl;

    public:
        ~PhysicsStateRestoreCandidate();
        PhysicsStateRestoreCandidate(PhysicsStateRestoreCandidate &&other) noexcept;
        PhysicsStateRestoreCandidate &operator=(PhysicsStateRestoreCandidate &&other) noexcept;
        PhysicsStateRestoreCandidate(const PhysicsStateRestoreCandidate &) = delete;
        PhysicsStateRestoreCandidate &operator=(const PhysicsStateRestoreCandidate &) = delete;
        [[nodiscard]] Result<void> ValidatePublication() const;
        void Publish() noexcept;

    private:
        friend struct Detail::PhysicsWorldSnapshotState;
        friend class PhysicsWorld;
        explicit PhysicsStateRestoreCandidate(std::shared_ptr<Impl> impl, std::uint64_t serial) noexcept;
        void Release() noexcept;
        std::shared_ptr<Impl> impl_;
        std::uint64_t serial_{};
    };

    /** @brief Checks exact supported compatibility representation, without granting
     * build or content authority.
     * @param compatibility Complete tuple. @return DescriptorInvalid on
     * missing/unknown fields.
     */
    [[nodiscard]] Result<void> ValidatePhysicsStateCompatibility(const PhysicsStateCompatibility &compatibility);
    /** @brief Validates every typed solver field, count, key, range and active-body
     * permutation without mutation.
     * @param state Complete candidate. @param limits Owner-admitted hard ceilings.
     * @return Success or DescriptorInvalid/CapacityExceeded; no native state is
     * touched.
     */
    [[nodiscard]] Result<void> ValidatePhysicsStateSolver(const PhysicsStateSolverView &state, const PhysicsStateLimits &limits,
                                                          std::span<std::uint8_t> activeBodyScratch);
}  // namespace Horo::Physics
