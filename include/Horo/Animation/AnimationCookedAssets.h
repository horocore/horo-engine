#pragma once

/** @file AnimationCookedAssets.h
 * @brief Animation-owned portable payload values; Assets retains envelopes, cache keys and publication authority.
 */
#include "Horo/Animation/AnimationClip.h"
#include "Horo/Animation/AnimationGraph.h"
#include "Horo/Assets/AssetDependency.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Sha256.h"

namespace Horo::Animation {
    struct AnimationGraphCookedData;

    /** @brief Closed domain payload vocabulary, independent from native backend or runtime ownership. */
    enum class AnimationCookedKind : std::uint32_t {
        Skeleton = 1,
        Clip = 2,
        Graph = 3
    };

    /** @brief Portable clip semantics with no process-local publication generation. */
    struct AnimationClipCookedData final {
        AnimationClipContractVersion contractVersion{CurrentAnimationClipContractVersion};
        AnimationClipId id;
        SkeletonId skeleton;
        AnimationDeltaTime duration;
        AnimationSampleRate sampleRate;
        AnimationWrapMode wrapMode{AnimationWrapMode::Once};
        AnimationClipKind kind{AnimationClipKind::Absolute};
        AnimationCompressionScheme compression{AnimationCompressionScheme::None};
        std::optional<AnimationReferencePoseId> referencePose;
        bool hasRootMotion{};
        std::vector<AnimationJointTrack> tracks;
        std::vector<AnimationSyncMarker> syncMarkers;
        std::vector<AnimationEventSchema> eventSchemas;
        std::vector<AnimationClipEvent> events;
    };

    /** @brief Actual immutable canonical domain payload supplied by the owning cook composition, never a naked digest. */
    struct AnimationCookedDependencyInput final {
        Assets::AssetDependency metadata;
        std::span<const std::uint8_t> payload;
    };

    /** @brief Stable role plus exact canonical domain content commitment; not an ID, generation or publication authority. */
    struct AnimationCookedDependency final {
        Assets::AssetDependency metadata;
        Sha256Digest payloadDigest;
    };

    /** @brief Fixed safety ceilings cannot be increased through a load/cook request. */
    struct AnimationCookedHardLimits final {
        static constexpr std::uint64_t Bytes = 256ULL * 1024ULL * 1024ULL;
        static constexpr std::uint64_t OwnedBytes = 512ULL * 1024ULL * 1024ULL;
        static constexpr std::uint64_t Values = 16ULL * 1024ULL * 1024ULL;
        static constexpr std::uint32_t Dependencies = 16384;
        static constexpr std::uint64_t DependencyBytes = 1024ULL * 1024ULL * 1024ULL;
        static constexpr std::uint32_t StringBytes = 4096;
    };

    /** @brief Caller-lowerable whole-payload byte, allocation and decoding-work policy. */
    struct AnimationCookedLimits final {
        std::uint64_t bytes{64ULL * 1024ULL * 1024ULL};
        std::uint64_t ownedBytes{256ULL * 1024ULL * 1024ULL};
        std::uint64_t values{8ULL * 1024ULL * 1024ULL};
        std::uint32_t dependencies{4096};
        std::uint64_t dependencyBytes{256ULL * 1024ULL * 1024ULL};
        std::uint32_t stringBytes{1024};
    };

    /** @brief Complete cold-boundary admission captured by the actual operation owner. */
    struct AnimationCookedContext final {
        AnimationCookedLimits limits;
        CancellationToken cancellation;
        bool accepting{true}; /**< False closes work; true grants neither project trust nor publication authority. */
    };

    /** @brief Encoded domain payload and exact semantic dependency declarations for the existing Assets cook sink. */
    struct AnimationCookedPayload final {
        Assets::AssetId asset;
        AnimationCookedKind kind;
        std::vector<Assets::AssetDependency> dependencies;
        std::vector<std::uint8_t> bytes;
    };

    namespace AnimationAssetErrors {
        extern const ErrorCodeDescriptor Malformed;
        extern const ErrorCodeDescriptor Unsupported;
        extern const ErrorCodeDescriptor Capacity;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor Unavailable;
        extern const ErrorCodeDescriptor Stale;
    }  // namespace AnimationAssetErrors

    /** @brief Encodes an already validated immutable skeleton, without an asset envelope or publication.
     * @param asset Actual immutable semantic source. @param context Finite cold-operation policy.
     * @return Complete canonical binary domain payload or typed cancellation/capacity/representation failure.
     * @details May allocate only under the declared cold policy; performs no I/O, cache or registry mutation.
     */
    [[nodiscard]] Result<AnimationCookedPayload> EncodeSkeletonAnimationPayload(const SkeletonAsset &asset,
                                                                                const AnimationCookedContext &context = {});

    /** @brief Decodes the exact expected stable skeleton identity through the existing SkeletonAsset validator.
     * @param bytes Animation-owned payload after Assets verifies its outer envelope and selected target/type.
     * @param expected Actual outer asset identity. @param context Finite cold-operation policy.
     * @return Fully validated immutable skeleton or typed failure; no active publication is modified.
     */
    [[nodiscard]] Result<SkeletonAsset> DecodeSkeletonAnimationPayload(std::span<const std::uint8_t> bytes, SkeletonId expected,
                                                                       const AnimationCookedContext &context = {});

    /** @brief Encodes complete portable clip semantics while excluding all publication generations.
     * @param asset Actual immutable validated clip. @param context Finite cold-operation policy.
     * @return Canonical payload plus exact skeleton/optional reference-pose roles or typed rejection.
     */
    [[nodiscard]] Result<AnimationCookedPayload> EncodeClipAnimationPayload(const AnimationClipAsset &asset,
                                                                            std::span<const std::uint8_t> skeletonPayload,
                                                                            const AnimationCookedContext &context = {});

    /** @brief Decodes portable clip data; the actual load owner must bind its exact skeleton/generations before Create.
     * @param bytes Domain payload from the selected verified Assets envelope. @param expected Actual outer clip identity.
     * @param context Finite cold-operation policy.
     * @return Complete typed portable data or failure, never a live clip or accepted publication.
     */
    [[nodiscard]] Result<AnimationClipCookedData> DecodeClipAnimationPayload(std::span<const std::uint8_t> bytes, AnimationClipId expected,
                                                                             const AnimationCookedContext &context = {});

    /** @brief Encodes the canonical compiled graph representation supplied by the actual graph owner.
     * @param program Actual immutable validated program; no runtime generations/occurrence storage are encoded.
     * @param context Finite cold-operation policy. @return Complete domain payload or typed failure.
     */
    [[nodiscard]] Result<AnimationCookedPayload> EncodeGraphAnimationPayload(const AnimationGraphProgram &program,
                                                                             std::span<const AnimationCookedDependencyInput> dependencies,
                                                                             const AnimationCookedContext &context = {});

    /** @brief Decodes typed compiled graph fields for the existing graph owner's FromCooked admission.
     * @param bytes Verified domain payload. @param expected Actual outer graph identity.
     * @param context Finite cold-operation policy. @return Detached compiled data or failure; no source parser or schedule selection.
     */
    [[nodiscard]] Result<AnimationGraphCookedData> DecodeGraphAnimationPayload(std::span<const std::uint8_t> bytes,
                                                                               AnimationGraphId expected,
                                                                               const AnimationCookedContext &context = {});
    /** @brief Reads checked cold dependency declarations for the existing Scene preparation loader.
     * @param bytes Domain payload from its actual verified Assets envelope. @param kind Exact selected semantic kind.
     * @param expected Actual outer identity. @param maximumReturnedDependencies Remaining charged-reference budget.
     * @param context Finite codec policy. @return Complete canonical declared roles or typed failure; grants no publication authority.
     * @details The native realization boundary separately verifies the complete semantic role table against actual decoded data.
     */
    [[nodiscard]] Result<std::vector<Assets::AssetDependency>> InspectAnimationPayloadDependencies(
        std::span<const std::uint8_t> bytes, AnimationCookedKind kind, Assets::AssetId expected, std::size_t maximumReturnedDependencies,
        const AnimationCookedContext &context = {});
    /** @brief Reads exact content commitments after the actual outer Assets envelope is verified.
     * @param bytes Domain payload. @param kind Exact semantic kind. @param expected Actual outer asset identity.
     * @param maximumReturnedDependencies Whole-operation remaining charged-reference bound. @param context Cold finite policy.
     * @return Canonical typed bindings; native realization must compare each payloadDigest with its actual verified HCAF dependency.
     */
    [[nodiscard]] Result<std::vector<AnimationCookedDependency>> InspectAnimationPayloadBindings(
        std::span<const std::uint8_t> bytes, AnimationCookedKind kind, Assets::AssetId expected, std::size_t maximumReturnedDependencies,
        const AnimationCookedContext &context = {});
}  // namespace Horo::Animation
