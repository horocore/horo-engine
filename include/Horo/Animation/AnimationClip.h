#pragma once

/**
 * @file AnimationClip.h
 * @brief Immutable animation clips, exact traversal, and allocation-free pose
 * sampling.
 */

#include "Horo/Animation/AnimationIdentity.h"
#include "Horo/Animation/SkeletonAsset.h"
#include "Horo/Math/SceneMath.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Animation {
    /** @brief Exact signed clip-local time in nanosecond ticks. */
    struct AnimationTime final {
        std::int64_t ticks{};
        [[nodiscard]] constexpr auto operator<=>(const AnimationTime &) const noexcept = default;
    };

    /** @brief Exact signed traversal amount in nanosecond ticks; negative values
     * request reverse playback. */
    struct AnimationTimeDelta final {
        std::int64_t ticks{};
        [[nodiscard]] constexpr auto operator<=>(const AnimationTimeDelta &) const noexcept = default;
    };

    /** @brief Exact positive clip duration in nanosecond ticks. */
    struct AnimationDeltaTime final {
        std::int64_t ticks{};
        [[nodiscard]] constexpr auto operator<=>(const AnimationDeltaTime &) const noexcept = default;
    };

    /** @brief Exact animation timebase resolution. */
    inline constexpr std::int64_t AnimationTicksPerSecond = 1'000'000'000;

    /** @brief Canonical positive rational authored samples-per-second value. */
    struct AnimationSampleRate final {
        std::uint32_t numerator{30};
        std::uint32_t denominator{1};
        [[nodiscard]] constexpr auto operator<=>(const AnimationSampleRate &) const noexcept = default;
    };

    /** @brief Exact portable animation-clip contract version. */
    struct AnimationClipContractVersion final {
        std::uint16_t major{};
        std::uint16_t minor{};
        std::uint16_t patch{};
        [[nodiscard]] constexpr auto operator<=>(const AnimationClipContractVersion &) const noexcept = default;
    };

    /** @brief Animation-clip version implemented by this API slice. */
    inline constexpr AnimationClipContractVersion CurrentAnimationClipContractVersion{1, 0, 0};

    /** @brief Playback behavior at the authored duration boundaries. */
    enum class AnimationWrapMode : std::uint8_t {
        Once,
        Loop,
        PingPong,
        ClampForever,
        Count
    };

    /** @brief Semantic representation of the stored joint transforms. */
    enum class AnimationClipKind : std::uint8_t {
        Absolute,
        Additive,
        Count
    };

    /** @brief Cook-time compression metadata; runtime keys are already decoded
     * portable values. */
    enum class AnimationCompressionScheme : std::uint8_t {
        None,
        Linear,
        Adaptive,
        Count
    };

    /** @brief Interpolation selected by the left transform key. */
    enum class AnimationInterpolation : std::uint8_t {
        Step,
        Linear,
        CubicHermite,
        Count
    };

    /** @brief Directed local traversal at the resulting sample. */
    enum class AnimationTraversalDirection : std::uint8_t {
        Held,
        Forward,
        Reverse,
        Count
    };

    /** @brief Stable identity and immutable generation of an additive clip's
     * reference pose. */
    struct AnimationReferencePoseBinding final {
        AnimationReferencePoseId id{};
        AnimationReferencePoseGeneration generation{};
        [[nodiscard]] constexpr auto operator<=>(const AnimationReferencePoseBinding &) const noexcept = default;
    };

    /** @brief One complete local-transform key at an exact clip-local time. */
    struct AnimationTransformKey final {
        AnimationTime time{};
        Math::Transform transform{};
        AnimationInterpolation interpolation{AnimationInterpolation::Linear};
        [[nodiscard]] constexpr bool operator==(const AnimationTransformKey &) const noexcept = default;
    };

    /** @brief One joint track keyed by stable skeleton-local identity. */
    struct AnimationJointTrack final {
        JointId joint{};
        std::vector<AnimationTransformKey> keys{};
        [[nodiscard]] bool operator==(const AnimationJointTrack &) const noexcept = default;
    };

    struct AnimationSyncMarkerTag;
    /** @brief Semantic marker name shared by compatible clip publications. */
    using AnimationSyncMarkerId = Foundation::Detail::NonZeroId64<AnimationSyncMarkerTag, AnimationErrors::IdentityInvalid>;

    /** @brief Immutable clip-local marker; exact times increase strictly in
     * [0,duration). */
    struct AnimationSyncMarker final {
        AnimationSyncMarkerId id{};
        AnimationTime time{};
        bool operator==(const AnimationSyncMarker &) const = default;
    };

    /** @brief Explicit player-local region within an immutable clip; the clip
     * descriptor stays unchanged. */
    struct AnimationClipRegion final {
        AnimationTime begin{};
        AnimationTime end{};
        AnimationWrapMode wrap{AnimationWrapMode::Once};
        bool operator==(const AnimationClipRegion &) const = default;
    };

    struct AnimationEventMarkerTag;
    struct AnimationEventTypeTag;
    struct AnimationEventFieldTag;
    /** @brief Stable clip-local authored event identity, independent of vector order or display name. */
    using AnimationEventMarkerId = Foundation::Detail::NonZeroId64<AnimationEventMarkerTag, AnimationErrors::IdentityInvalid>;
    /** @brief Registered semantic event identity supplied by the owning cook/application adapter. */
    using AnimationEventTypeId = Foundation::Detail::NonZeroId64<AnimationEventTypeTag, AnimationErrors::IdentityInvalid>;
    /** @brief Stable schema-local typed payload field identity. */
    using AnimationEventFieldId = Foundation::Detail::NonZeroId64<AnimationEventFieldTag, AnimationErrors::IdentityInvalid>;
    /** @brief Closed portable event field vocabulary; no strings, callbacks, native handles or owning objects. */
    enum class AnimationEventValueKind : std::uint8_t {
        Boolean,
        SignedInteger,
        UnsignedInteger,
        Number,
        Vector3,
        Asset,
        Count
    };
    /** @brief Fixed value storage whose copies remain valid after clip/instance retirement. */
    using AnimationEventValue = std::variant<bool, std::int64_t, std::uint64_t, double, Math::Vec3, Assets::AssetId>;

    /** @brief One typed map entry; field ID is authoritative. */
    struct AnimationEventField final {
        AnimationEventFieldId id{};
        AnimationEventValue value{false};
        bool operator==(const AnimationEventField &) const = default;
    };

    /** @brief Fixed-capacity typed field map; unused entries must retain their default value. */
    struct AnimationEventPayload final {
        static constexpr std::uint32_t MaximumFields = 8;
        std::array<AnimationEventField, MaximumFields> fields{};
        std::uint32_t count{};
        bool operator==(const AnimationEventPayload &) const = default;
    };

    /** @brief Cook-time field declaration; optional unsigned allowlists encode adjacent owner's finite registered values. */
    struct AnimationEventFieldSchema final {
        AnimationEventFieldId id{};
        AnimationEventValueKind kind{AnimationEventValueKind::Boolean};
        bool required{true};
        std::vector<std::uint64_t> allowedUnsigned{};
        bool operator==(const AnimationEventFieldSchema &) const = default;
    };

    /** @brief Immutable typed payload schema copied into this exact clip revision, not a runtime service. */
    struct AnimationEventSchema final {
        AnimationEventTypeId type{};
        std::uint32_t revision{1};
        std::vector<AnimationEventFieldSchema> fields{};
        bool operator==(const AnimationEventSchema &) const = default;
    };

    /** @brief Authored point or half-open duration window [time,time+duration), owned by the actual clip publication. */
    struct AnimationClipEvent final {
        AnimationEventMarkerId id{};
        std::string name{};
        AnimationEventTypeId type{};
        std::uint32_t schemaRevision{1};
        AnimationTime time{};
        std::optional<AnimationDeltaTime> duration{};
        AnimationEventPayload payload{};
        bool fireInReverse{};
        bool fireOnActivation{true}; /**< Includes the first actual travelled origin; holding/sampling alone produces no point. */
        bool operator==(const AnimationClipEvent &) const = default;
    };

    /** @brief Immutable identity, binding, time, playback, and storage metadata for
     * one clip publication. */
    struct AnimationClipDescriptor final {
        AnimationClipContractVersion contractVersion{CurrentAnimationClipContractVersion};
        AnimationClipId id{};
        AnimationClipGeneration generation{};
        SkeletonId skeleton{};
        SkeletonAssetGeneration skeletonGeneration{};
        AnimationDeltaTime duration{};
        AnimationSampleRate sampleRate{};
        AnimationWrapMode wrapMode{AnimationWrapMode::Once};
        AnimationClipKind kind{AnimationClipKind::Absolute};
        AnimationCompressionScheme compression{AnimationCompressionScheme::None};
        std::optional<AnimationReferencePoseBinding> referencePose{};
        bool hasRootMotion{};
        [[nodiscard]] auto operator<=>(const AnimationClipDescriptor &) const noexcept = default;
    };

    /** @brief Mutable clip candidate copied into an immutable canonical snapshot
     * after validation. */
    struct AnimationClipData final {
        AnimationClipDescriptor descriptor{};
        std::vector<AnimationJointTrack> tracks{};
        std::vector<AnimationSyncMarker> syncMarkers{}; /**< Pinned with this exact clip generation; never a runtime side table. */
        std::vector<AnimationEventSchema> eventSchemas{};
        std::vector<AnimationClipEvent> events{};
        [[nodiscard]] bool operator==(const AnimationClipData &) const noexcept = default;
    };

    /** @brief Compile-time safety ceilings for one portable clip and one traversal.
     */
    struct AnimationClipHardLimits final {
        static constexpr std::uint32_t SyncMarkers = 256;
        static constexpr std::uint32_t Events = 1024;
        static constexpr std::uint32_t EventSchemas = 64;
        static constexpr std::uint32_t EventNameBytes = 64;
        static constexpr std::uint32_t AllowedEventValues = 32;
        static constexpr std::uint32_t Tracks = SkeletonAssetHardLimits::Joints;
        static constexpr std::uint32_t KeysPerTrack = 65'536;
        static constexpr std::uint32_t TotalKeys = 1'048'576;
        static constexpr std::uint32_t BoundaryCrossings = 4'096;
        static constexpr std::uint32_t TraceSegments = BoundaryCrossings + 2;
        static constexpr std::uint32_t SampleRateNumerator = 240'000;
        static constexpr std::uint32_t SampleRateDenominator = 10'000;
        static constexpr std::int64_t DurationTicks = 24LL * 60LL * 60LL * AnimationTicksPerSecond;
    };

    /** @brief Finite policy captured before clip validation or traversal begins. */
    struct AnimationClipLimits final {
        std::uint32_t maximumTracks{AnimationClipHardLimits::Tracks};
        std::uint32_t maximumKeysPerTrack{AnimationClipHardLimits::KeysPerTrack};
        std::uint32_t maximumTotalKeys{AnimationClipHardLimits::TotalKeys};
        std::uint32_t maximumBoundaryCrossings{64};
        std::uint32_t maximumEvents{AnimationClipHardLimits::Events};
        std::uint32_t maximumEventSchemas{AnimationClipHardLimits::EventSchemas};
        [[nodiscard]] constexpr auto operator<=>(const AnimationClipLimits &) const noexcept = default;
    };

    /** @brief Captured owner state used to fail closed before validation,
     * traversal, or sampling. */
    enum class AnimationClipAdmissionState : std::uint8_t {
        Accepting,
        CancellationRequested,
        ShuttingDown,
        Count
    };

    /** @brief Immutable operation inputs captured by the clip owner before
     * validation. */
    struct AnimationClipBuildContext final {
        AnimationClipAdmissionState admission{AnimationClipAdmissionState::Accepting};
        std::optional<AnimationClipId> replacing{};
        AnimationClipLimits limits{};
    };

    /** @brief Canonical bounded player state; phase is normalized for the
     * descriptor's wrap mode. */
    struct AnimationClipCursor final {
        AnimationTime phase{};
        bool terminal{};
        [[nodiscard]] constexpr auto operator<=>(const AnimationClipCursor &) const noexcept = default;
    };

    /** @brief Atomic result of one bounded directed cursor traversal. */
    struct AnimationClipTraversal final {
        AnimationClipCursor cursor{};
        AnimationTime sampleTime{};
        AnimationTraversalDirection direction{AnimationTraversalDirection::Held};
        std::uint32_t boundaryCrossings{};
        [[nodiscard]] constexpr auto operator<=>(const AnimationClipTraversal &) const noexcept = default;
    };

    /**
     * @brief Caller-owned immutable and mutable views required for one frame-hot
     * sample.
     *
     * Additive output may exactly alias the complete base pose for in-place
     * application. Partial base/output overlap and any reference/output overlap are
     * rejected before output mutation.
     */
    struct AnimationClipSampleContext final {
        AnimationClipAdmissionState admission{AnimationClipAdmissionState::Accepting};
        SkeletonId skeleton{};
        SkeletonAssetGeneration skeletonGeneration{};
        AnimationClipGeneration clipGeneration{};
        std::optional<AnimationReferencePoseBinding> referencePose{};
        std::span<const Math::Transform> basePose{};
        std::span<const Math::Transform> referenceLocalPose{};
        std::span<Math::Transform> outputPose{};
    };

    /** @brief Metadata identifying one completed allocation-free pose sample. */
    struct AnimationClipSample final {
        AnimationTime time{};
        std::uint32_t sampledTracks{};
        bool additive{};
        [[nodiscard]] constexpr auto operator<=>(const AnimationClipSample &) const noexcept = default;
    };

    /** @brief Allocation-free clip failure vocabulary; diagnostic codes are
     * projected outside the hot path. */
    enum class AnimationClipFailure : std::uint8_t {
        None,
        Cancelled,
        AdmissionRejected,
        IdentityInvalid,
        LimitExceeded,
        Malformed,
        Unsupported,
        TimeOverflow,
        SkeletonMismatch,
        BindingStale,
        ReferencePoseMismatch
    };

    /** @brief Immutable failure evidence; detail refers only to library-owned
     * string literals. */
    struct AnimationClipFailureInfo final {
        AnimationClipFailure failure{AnimationClipFailure::None};
        std::string_view detail{};
    };

    /** @brief Clip-specific bounded result that owns no diagnostic string or
     * allocation. */
    template <typename T> struct AnimationClipOutcome final {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        AnimationClipFailureInfo failure{};

        [[nodiscard]] static AnimationClipOutcome Success(T value) noexcept {
            return {std::move(value), {}};
        }

        [[nodiscard]] static AnimationClipOutcome Failure(AnimationClipFailureInfo failure) noexcept {
            return {{}, failure};
        }

        [[nodiscard]] bool HasValue() const noexcept {
            return failure.failure == AnimationClipFailure::None;
        }

        [[nodiscard]] bool HasError() const noexcept {
            return !HasValue();
        }

        [[nodiscard]] const T &Value() const noexcept {
            return value;
        }

        [[nodiscard]] AnimationClipFailureInfo ErrorValue() const noexcept {
            return failure;
        }
    };

    /** @brief Allocation-free validation outcome used by the same prepared clip
     * validators. */
    template <> struct AnimationClipOutcome<void> final {
        AnimationClipFailureInfo failure{};

        [[nodiscard]] static AnimationClipOutcome Success() noexcept {
            return {};
        }

        [[nodiscard]] static AnimationClipOutcome Failure(AnimationClipFailureInfo failure) noexcept {
            return {failure};
        }

        [[nodiscard]] bool HasValue() const noexcept {
            return failure.failure == AnimationClipFailure::None;
        }

        [[nodiscard]] bool HasError() const noexcept {
            return !HasValue();
        }

        [[nodiscard]] AnimationClipFailureInfo ErrorValue() const noexcept {
            return failure;
        }
    };

    /** @brief One actual directed sample interval, split only by the canonical clip traversal provider. */
    struct AnimationClipTraversalSegment final {
        AnimationTime from{};
        AnimationTime to{};
        AnimationTraversalDirection direction{AnimationTraversalDirection::Held};
        bool loopSeamBefore{}; /**< Reverse Loop departure from canonical phase zero enters the preceding cycle. */
        bool loopSeamAfter{};
        std::int8_t cycleDeltaBefore{}; /**< Checked reverse departure from cyclic phase zero; no independent clock. */
        std::int8_t cycleDeltaAfter{};
        /**< Checked forward arrival at cyclic phase zero; PingPong remains continuous. */ /**< Forward Loop arrival at duration enters
                                                                                              the next canonical cycle. */
    };

    /** @brief Exact complete provider outcome and count in caller-owned segment storage. */
    struct AnimationClipTrace final {
        AnimationClipTraversal traversal{};
        std::uint32_t segments{};
    };

    /** @brief Converts typed hot failure to the original owned diagnostic at a
     * control boundary.
     * @param failure Library-owned typed failure and literal context.
     * @return Original stable code, domain, severity and diagnostic context; may
     * allocate.
     * @pre failure.failure is not None.
     */
    [[nodiscard]] Error DescribeAnimationClipFailure(AnimationClipFailureInfo failure);

    /**
     * @brief Immutable validated animation clip with canonical tracks and
     * pre-resolved skeleton indices.
     *
     * Create is a load/cook/control-boundary transaction and may allocate. Prepared
     * operations are history-independent, thread-safe, bounded, allocation-free on
     * all outcomes, and perform no blocking I/O while this object, its immutable
     * skeleton publication, and caller-provided spans remain alive. Legacy Result
     * wrappers use the same kernels and may allocate only when projecting failure
     * diagnostics.
     */
    class AnimationClipAsset final {
    public:
        /**
         * @brief Validates and takes ownership of a complete animation clip
         * transactionally.
         * @param candidate Candidate descriptor and joint tracks.
         * @param skeleton Exact immutable skeleton publication referenced by the
         * descriptor.
         * @param context Captured lifecycle, reload identity, and finite limits.
         * @return Immutable canonical clip or a stable version, binding, malformed,
         * duplicate, unsupported, or limit failure.
         * @pre Load/cook/control boundary; never invoke from frame-hot animation
         * evaluation.
         * @post Failure publishes no partial asset or replacement.
         */
        [[nodiscard]] static Result<AnimationClipAsset> Create(AnimationClipData candidate, const SkeletonAsset &skeleton,
                                                               const AnimationClipBuildContext &context = {});

        /** @brief Returns immutable canonical clip data. @return Borrowed data owned
         * by this asset. */
        [[nodiscard]] const AnimationClipData &Data() const noexcept;

        /**
         * @brief Advances one canonical cursor by an exact signed amount without
         * mutating player state.
         * @param cursor Current cursor previously initialized to zero or returned by
         * this clip.
         * @param delta Exact signed player-local traversal amount.
         * @param limits Captured finite crossing policy; hard ceilings always apply.
         * @param admission Captured owner lifecycle state.
         * @return Complete candidate cursor/sample metadata or a stable lifecycle,
         * cursor, overflow, or budget failure.
         * @post Failure leaves caller-owned player state unchanged; success allocates
         * nothing.
         */
        [[nodiscard]] Result<AnimationClipTraversal> Traverse(
            AnimationClipCursor cursor, AnimationTimeDelta delta, const AnimationClipLimits &limits = {},
            AnimationClipAdmissionState admission = AnimationClipAdmissionState::Accepting) const;

        /**
         * @brief Samples a complete local pose into caller-owned storage at an
         * in-range clip time.
         * @param time Exact local time in the closed interval `[0, duration]`.
         * @param context Exact asset-generation bindings and non-overlapping pose
         * views, except that additive output may exactly alias base.
         * @return Sample metadata or a stable lifecycle, stale, binding,
         * malformed-pose, time, or size failure.
         * @post Success allocates nothing and writes every output joint; failure
         * performs no output writes.
         */
        [[nodiscard]] Result<AnimationClipSample> Sample(AnimationTime time, const AnimationClipSampleContext &context) const;

        /** @brief Executes the same exact traversal with allocation-free failure
         * evidence.
         * @param cursor Canonical current cursor.
         * @param delta Exact signed player-local traversal amount.
         * @param limits Captured finite crossing policy.
         * @param admission Captured lifecycle state.
         * @return Complete candidate traversal or typed failure; no state mutation or
         * allocation.
         */
        [[nodiscard]] AnimationClipOutcome<AnimationClipTraversal> TraversePrepared(
            AnimationClipCursor cursor, AnimationTimeDelta delta, const AnimationClipLimits &limits = {},
            AnimationClipAdmissionState admission = AnimationClipAdmissionState::Accepting) const;

        /** @brief Traverses one admitted region using the same checked clip traversal
         * kernel.
         * @param cursor Exact region-relative cursor (phase origin is region.begin).
         * @param delta Signed exact traversal interval; reverse eligibility belongs
         * to the caller.
         * @param region Exact nonempty interval within this clip and explicit player
         * wrap policy.
         * @param limits Finite traversal/boundary policy; implementation hard limits
         * cannot be raised.
         * @param admission Captured owner admission state, checked before region or
         * cursor access.
         * @return Region-relative next cursor and absolute clip-local sample time, or
         * typed failure.
         * @pre The immutable clip and caller's captured owner admission remain valid
         * for this call.
         * @post This clip's descriptor, tracks and marker publication remain
         * unchanged. No allocation or I/O.
         */
        [[nodiscard]] AnimationClipOutcome<AnimationClipTraversal> TraverseRegionPrepared(
            AnimationClipCursor cursor, AnimationTimeDelta delta, AnimationClipRegion region, const AnimationClipLimits &limits = {},
            AnimationClipAdmissionState admission = AnimationClipAdmissionState::Accepting) const;

        /** @brief Exposes the complete actual directed path through the same checked regional traversal kernel.
         * @param cursor Exact region-relative starting cursor. @param delta Complete signed interval.
         * @param region Exact clip-local region/wrap policy. @param output Caller-owned bounded segment storage.
         * @param limits Existing boundary policy, checked before output writes. @param admission Captured owner admission.
         * @return Exact TraverseRegionPrepared result plus complete segment count; failure leaves output untouched.
         * @post No allocation/I/O; segments conserve actual travelled path, including loop seams and PingPong turns.
         */
        [[nodiscard]] AnimationClipOutcome<AnimationClipTrace> TraceRegionPrepared(
            AnimationClipCursor cursor, AnimationTimeDelta delta, AnimationClipRegion region,
            std::span<AnimationClipTraversalSegment> output, const AnimationClipLimits &limits = {},
            AnimationClipAdmissionState admission = AnimationClipAdmissionState::Accepting) const;

        /** @brief Executes the same pose sample with allocation-free failure
         * evidence.
         * @param time Exact in-range clip time.
         * @param context Exact immutable dependencies and caller-owned pose spans.
         * @return Complete sample or typed failure before writes; allocates nothing.
         */
        [[nodiscard]] AnimationClipOutcome<AnimationClipSample> SamplePrepared(AnimationTime time,
                                                                               const AnimationClipSampleContext &context) const;

    private:
        AnimationClipAsset(AnimationClipData data, std::vector<std::uint32_t> jointIndices,
                           std::vector<Math::Transform> skeletonReferencePose) noexcept
            : data_(std::move(data)), jointIndices_(std::move(jointIndices)), skeletonReferencePose_(std::move(skeletonReferencePose)) {}

        AnimationClipData data_;
        std::vector<std::uint32_t> jointIndices_;
        std::vector<Math::Transform> skeletonReferencePose_;
    };
}  // namespace Horo::Animation
