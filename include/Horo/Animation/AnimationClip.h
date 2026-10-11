#pragma once

/**
 * @file AnimationClip.h
 * @brief Immutable animation clips, exact traversal, and allocation-free pose sampling.
 */

#include "Horo/Animation/AnimationIdentity.h"
#include "Horo/Animation/SkeletonAsset.h"
#include "Horo/Math/SceneMath.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Animation {
    /** @brief Exact signed clip-local time in nanosecond ticks. */
    struct AnimationTime final {
        std::int64_t ticks{};
        [[nodiscard]] constexpr auto operator<=>(const AnimationTime &) const noexcept = default;
    };

    /** @brief Exact signed traversal amount in nanosecond ticks; negative values request reverse playback. */
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

    /** @brief Cook-time compression metadata; runtime keys are already decoded portable values. */
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

    /** @brief Stable identity and immutable generation of an additive clip's reference pose. */
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

    /** @brief Immutable identity, binding, time, playback, and storage metadata for one clip publication. */
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

    /** @brief Mutable clip candidate copied into an immutable canonical snapshot after validation. */
    struct AnimationClipData final {
        AnimationClipDescriptor descriptor{};
        std::vector<AnimationJointTrack> tracks{};
        [[nodiscard]] bool operator==(const AnimationClipData &) const noexcept = default;
    };

    /** @brief Compile-time safety ceilings for one portable clip and one traversal. */
    struct AnimationClipHardLimits final {
        static constexpr std::uint32_t Tracks = SkeletonAssetHardLimits::Joints;
        static constexpr std::uint32_t KeysPerTrack = 65'536;
        static constexpr std::uint32_t TotalKeys = 1'048'576;
        static constexpr std::uint32_t BoundaryCrossings = 4'096;
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
        [[nodiscard]] constexpr auto operator<=>(const AnimationClipLimits &) const noexcept = default;
    };

    /** @brief Captured owner state used to fail closed before validation, traversal, or sampling. */
    enum class AnimationClipAdmissionState : std::uint8_t {
        Accepting,
        CancellationRequested,
        ShuttingDown,
        Count
    };

    /** @brief Immutable operation inputs captured by the clip owner before validation. */
    struct AnimationClipBuildContext final {
        AnimationClipAdmissionState admission{AnimationClipAdmissionState::Accepting};
        std::optional<AnimationClipId> replacing{};
        AnimationClipLimits limits{};
    };

    /** @brief Canonical bounded player state; phase is normalized for the descriptor's wrap mode. */
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
     * @brief Caller-owned immutable and mutable views required for one frame-hot sample.
     *
     * Additive output may exactly alias the complete base pose for in-place application. Partial
     * base/output overlap and any reference/output overlap are rejected before output mutation.
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

    /** @brief Allocation-free clip failure vocabulary; diagnostic codes are projected outside the hot path. */
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

    /** @brief Immutable failure evidence; detail refers only to library-owned string literals. */
    struct AnimationClipFailureInfo final {
        AnimationClipFailure failure{AnimationClipFailure::None};
        std::string_view detail{};
    };

    /** @brief Clip-specific bounded result that owns no diagnostic string or allocation. */
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

    /** @brief Allocation-free validation outcome used by the same prepared clip validators. */
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

    /** @brief Converts typed hot failure to the original owned diagnostic at a control boundary.
     * @param failure Library-owned typed failure and literal context.
     * @return Original stable code, domain, severity and diagnostic context; may allocate.
     * @pre failure.failure is not None.
     */
    [[nodiscard]] Error DescribeAnimationClipFailure(AnimationClipFailureInfo failure);

    /**
     * @brief Immutable validated animation clip with canonical tracks and pre-resolved skeleton indices.
     *
     * Create is a load/cook/control-boundary transaction and may allocate. Prepared operations are
     * history-independent, thread-safe, bounded, allocation-free on all outcomes, and perform no blocking I/O while
     * this object, its immutable skeleton publication, and caller-provided spans remain alive. Legacy
     * Result wrappers use the same kernels and may allocate only when projecting failure diagnostics.
     */
    class AnimationClipAsset final {
    public:
        /**
         * @brief Validates and takes ownership of a complete animation clip transactionally.
         * @param candidate Candidate descriptor and joint tracks.
         * @param skeleton Exact immutable skeleton publication referenced by the descriptor.
         * @param context Captured lifecycle, reload identity, and finite limits.
         * @return Immutable canonical clip or a stable version, binding, malformed, duplicate, unsupported, or limit failure.
         * @pre Load/cook/control boundary; never invoke from frame-hot animation evaluation.
         * @post Failure publishes no partial asset or replacement.
         */
        [[nodiscard]] static Result<AnimationClipAsset> Create(AnimationClipData candidate, const SkeletonAsset &skeleton,
                                                               const AnimationClipBuildContext &context = {});

        /** @brief Returns immutable canonical clip data. @return Borrowed data owned by this asset. */
        [[nodiscard]] const AnimationClipData &Data() const noexcept;

        /**
         * @brief Advances one canonical cursor by an exact signed amount without mutating player state.
         * @param cursor Current cursor previously initialized to zero or returned by this clip.
         * @param delta Exact signed player-local traversal amount.
         * @param limits Captured finite crossing policy; hard ceilings always apply.
         * @param admission Captured owner lifecycle state.
         * @return Complete candidate cursor/sample metadata or a stable lifecycle, cursor, overflow, or budget failure.
         * @post Failure leaves caller-owned player state unchanged; success allocates nothing.
         */
        [[nodiscard]] Result<AnimationClipTraversal> Traverse(
            AnimationClipCursor cursor, AnimationTimeDelta delta, const AnimationClipLimits &limits = {},
            AnimationClipAdmissionState admission = AnimationClipAdmissionState::Accepting) const;

        /**
         * @brief Samples a complete local pose into caller-owned storage at an in-range clip time.
         * @param time Exact local time in the closed interval `[0, duration]`.
         * @param context Exact asset-generation bindings and non-overlapping pose views, except that additive output may exactly alias
         * base.
         * @return Sample metadata or a stable lifecycle, stale, binding, malformed-pose, time, or size failure.
         * @post Success allocates nothing and writes every output joint; failure performs no output writes.
         */
        [[nodiscard]] Result<AnimationClipSample> Sample(AnimationTime time, const AnimationClipSampleContext &context) const;

        /** @brief Executes the same exact traversal with allocation-free failure evidence.
         * @param cursor Canonical current cursor.
         * @param delta Exact signed player-local traversal amount.
         * @param limits Captured finite crossing policy.
         * @param admission Captured lifecycle state.
         * @return Complete candidate traversal or typed failure; no state mutation or allocation.
         */
        [[nodiscard]] AnimationClipOutcome<AnimationClipTraversal> TraversePrepared(
            AnimationClipCursor cursor, AnimationTimeDelta delta, const AnimationClipLimits &limits = {},
            AnimationClipAdmissionState admission = AnimationClipAdmissionState::Accepting) const;

        /** @brief Executes the same pose sample with allocation-free failure evidence.
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
