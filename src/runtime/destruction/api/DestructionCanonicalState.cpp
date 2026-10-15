#include "Horo/Destruction/DestructionCanonicalState.h"

#include <algorithm>
#include <ranges>

namespace Horo::Destruction {
    namespace {
        constexpr std::size_t CanonicalFixedBytes = 141;

        /** @brief Preserve a stable semantic validation rejection without translating its descriptor. */
        template <typename T> Result<T> Fail(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Check product bounds against fixed semantic storage ceilings before inspecting collections. */
        bool ValidLimits(const DestructionCanonicalLimits &limits) noexcept {
            return limits.maximumChunks > 0 && limits.maximumChunks <= DestructionHardLimits::ChunksPerDestructible &&
                   limits.maximumAnchors <= DestructionHardLimits::ChunksPerDestructible && limits.maximumBytes >= CanonicalFixedBytes &&
                   limits.maximumBytes <= 16 * 1024;
        }

        /** @brief Check whether a bounded semantic set has any members. */
        [[nodiscard]] bool AnyBits(const std::span<const std::byte> mask) noexcept {
            return std::ranges::any_of(mask, [](const std::byte value) {
                return value != std::byte{};
            });
        }

        /** @brief Enforce exact byte length and unused high bits for an artifact-scoped set. */
        [[nodiscard]] bool ValidMask(const std::span<const std::byte> mask, const std::uint32_t count) noexcept {
            if (const std::size_t bytes = (static_cast<std::size_t>(count) + 7U) / 8U; mask.size() != bytes)
                return false;
            if (count % 8U == 0)
                return true;
            const auto highBits = std::byte{0xFF} << (count % 8U);
            return (mask.back() & highBits) == std::byte{};
        }

    }  // namespace

    /** @copydoc ValidateCanonicalDestructionSnapshot */
    [[nodiscard]] Result<void> ValidateCanonicalDestructionSnapshot(const CanonicalDestructionSnapshot &state,
                                                                    const DestructionCanonicalArtifactView &artifact,
                                                                    const DestructionCanonicalLimits &limits) {
        if (!ValidLimits(limits))
            return Fail<void>(DestructionErrors::ReplicationLimitExceeded);
        if (state.schemaVersion != 1 || !state.target.IsValid() || !state.content.IsValid() || !state.configuration.IsValid() ||
            !state.effectiveFeatures.IsValid() || !state.authority.IsValid() || !state.revision.IsValid() || state.seed.version == 0 ||
            state.phase > DestructionStatePhase::Destroyed)
            return Fail<void>(DestructionErrors::ReplicationInvalid);
        if (state.masks.chunkCount == 0 || state.masks.chunkCount > limits.maximumChunks ||
            state.supportAnchors.size() > limits.maximumAnchors)
            return Fail<void>(DestructionErrors::ReplicationLimitExceeded);
        if (artifact.content != state.content || artifact.canonicalChunks.size() != state.masks.chunkCount)
            return Fail<void>(DestructionErrors::ReplicationIncompatible);
        if (state.supportCursor > state.masks.chunkCount || !std::ranges::is_sorted(artifact.canonicalChunks) ||
            std::ranges::adjacent_find(artifact.canonicalChunks) != artifact.canonicalChunks.end() ||
            std::ranges::any_of(artifact.canonicalChunks, [](const auto id) {
            return !id.IsValid();
        }))
            return Fail<void>(DestructionErrors::ReplicationInvalid);
        const auto &masks = state.masks;
        if (!ValidMask(masks.broken, masks.chunkCount) || !ValidMask(masks.active, masks.chunkCount) ||
            !ValidMask(masks.supported, masks.chunkCount) || !ValidMask(masks.dormant, masks.chunkCount))
            return Fail<void>(DestructionErrors::ReplicationInvalidChunkMask);
        for (std::size_t index = 0; index < masks.broken.size(); ++index) {
            if ((masks.active[index] & masks.dormant[index]) != std::byte{} ||
                ((masks.active[index] | masks.dormant[index]) & ~masks.broken[index]) != std::byte{})
                return Fail<void>(DestructionErrors::ReplicationInvalidChunkMask);
        }
        if (state.phase == DestructionStatePhase::Intact && (AnyBits(masks.broken) || AnyBits(masks.active) || AnyBits(masks.dormant)))
            return Fail<void>(DestructionErrors::ReplicationInvalidChunkMask);
        if (!std::ranges::is_sorted(state.supportAnchors) ||
            std::ranges::adjacent_find(state.supportAnchors) != state.supportAnchors.end() ||
            std::ranges::any_of(state.supportAnchors, [](const DestructionChunkId id) {
            return !id.IsValid();
        }))
            return Fail<void>(DestructionErrors::ReplicationInvalid);
        for (const DestructionChunkId anchor : state.supportAnchors) {
            if (std::ranges::find(artifact.canonicalChunks, anchor) == artifact.canonicalChunks.end())
                return Fail<void>(DestructionErrors::ReplicationIncompatible);
        }
        if (const std::size_t payloadBytes = CanonicalFixedBytes + 4U * masks.broken.size() + 8U * state.supportAnchors.size();
            payloadBytes > limits.maximumBytes)
            return Fail<void>(DestructionErrors::ReplicationLimitExceeded);
        return Result<void>::Success();
    }

}  // namespace Horo::Destruction
