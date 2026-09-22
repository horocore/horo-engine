#pragma once

/**
 * @file CinematicIdentity.h
 * @brief Stable generation-safe cinematic identities and canonical persistence encoding.
 */

#include "Horo/Cinematic/CinematicErrors.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/StableIdentity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Horo::Cinematic {
    /** @brief Canonical network-byte-order encoding of a stable value and generation. */
    using SerializedCinematicIdentity = std::array<std::uint8_t, 12>;

    namespace Detail {
        /** @brief Writes one unsigned integer to a validated slice of the canonical identity representation. */
        template <typename Integer>
        constexpr void WriteNetworkOrder(SerializedCinematicIdentity &bytes, const std::size_t offset, Integer value) noexcept {
            static_assert(std::is_unsigned_v<Integer>);
            for (std::size_t byte = 0; byte < sizeof(Integer); ++byte) {
                const std::size_t shift = (sizeof(Integer) - byte - 1U) * 8U;
                bytes[offset + byte] = static_cast<std::uint8_t>(value >> shift);
            }
        }

        /** @brief Reads one unsigned integer from a validated slice of the canonical identity representation. */
        template <typename Integer>
        [[nodiscard]] constexpr Integer ReadNetworkOrder(const SerializedCinematicIdentity &bytes, const std::size_t offset) noexcept {
            static_assert(std::is_unsigned_v<Integer>);
            Integer value{};
            for (std::size_t byte = 0; byte < sizeof(Integer); ++byte)
                value = static_cast<Integer>((value << 8U) | bytes[offset + byte]);
            return value;
        }
    }  // namespace Detail

    /** @brief Strong generation-safe identity in one tag-defined cinematic domain. */
    template <typename Tag> using CinematicIdentity = Foundation::StableIdentity<Tag>;

    struct SequenceIdentityTag;
    struct TrackIdentityTag;
    struct KeyframeIdentityTag;
    struct TransformBindingIdentityTag;
    struct RuntimeSessionIdentityTag;
    struct SequencePlayerIdentityTag;

    /** @brief Stable generation-safe identity of one authored cinematic sequence. */
    using SequenceId = CinematicIdentity<SequenceIdentityTag>;
    /** @brief Stable generation-safe identity of one track in authored sequence data. */
    using TrackId = CinematicIdentity<TrackIdentityTag>;
    /** @brief Stable generation-safe identity of one keyframe in authored sequence data. */
    using KeyframeId = CinematicIdentity<KeyframeIdentityTag>;
    /** @brief Stable generation-safe identity of one declared property binding. */
    using PropertyBindingIdentityTag = Foundation::PropertyBindingIdentityTag;
    using PropertyBindingId = Foundation::PropertyBindingId;
    /** @brief Stable generation-safe identity of one scene transform binding. */
    using TransformBindingId = CinematicIdentity<TransformBindingIdentityTag>;
    /** @brief Stable generation-safe identity of one cinematic runtime session. */
    using CinematicRuntimeSessionId = CinematicIdentity<RuntimeSessionIdentityTag>;
    /** @brief Stable generation-safe identity of one registry-owned sequence player. */
    using SequencePlayerId = CinematicIdentity<SequencePlayerIdentityTag>;

    /**
     * @brief Creates a validated cinematic identity from owner-issued dimensions.
     * @param stableValue Durable non-zero authored value.
     * @param generation Non-zero current generation.
     * @return Typed identity or CinematicErrors::IdentityInvalid.
     */
    template <typename Tag>
    [[nodiscard]] Result<CinematicIdentity<Tag>> MakeCinematicIdentity(const std::uint64_t stableValue, const std::uint32_t generation) {
        const CinematicIdentity<Tag> identity{stableValue, generation};
        if (!identity.IsValid())
            return Result<CinematicIdentity<Tag>>::Failure(MakeError(CinematicErrors::IdentityInvalid));
        return Result<CinematicIdentity<Tag>>::Success(identity);
    }

    /**
     * @brief Encodes an identity in a fixed-width canonical network-byte-order representation.
     * @param identity Identity to persist through editor, save/load, and cook boundaries.
     * @return Exact bytes; an invalid identity encodes reserved values and fails decoding.
     */
    template <typename Tag>
    [[nodiscard]] constexpr SerializedCinematicIdentity SerializeCinematicIdentity(const CinematicIdentity<Tag> identity) noexcept {
        SerializedCinematicIdentity bytes{};
        Detail::WriteNetworkOrder(bytes, 0, identity.stableValue);
        Detail::WriteNetworkOrder(bytes, sizeof(identity.stableValue), identity.generation);
        return bytes;
    }

    /**
     * @brief Decodes and validates one canonical cinematic identity.
     * @param bytes Fixed-width network-byte-order representation.
     * @return Exact typed identity or CinematicErrors::SerializedIdentityInvalid.
     */
    template <typename Tag>
    [[nodiscard]] Result<CinematicIdentity<Tag>> DeserializeCinematicIdentity(const SerializedCinematicIdentity &bytes) {
        auto identity = MakeCinematicIdentity<Tag>(Detail::ReadNetworkOrder<std::uint64_t>(bytes, 0),
                                                   Detail::ReadNetworkOrder<std::uint32_t>(bytes, sizeof(std::uint64_t)));
        if (identity.HasError())
            return Result<CinematicIdentity<Tag>>::Failure(MakeError(CinematicErrors::SerializedIdentityInvalid));
        return identity;
    }

    /**
     * @brief Validates a submitted identity against the owning document's exact current identity.
     * @param submitted Candidate supplied by an editor or runtime consumer.
     * @param current Current identity stored by the owner.
     * @return Success, IdentityInvalid, IdentityUnknown, or IdentityStale.
     */
    template <typename Tag>
    [[nodiscard]] Result<void> ValidateCinematicIdentityAccess(const CinematicIdentity<Tag> submitted,
                                                               const CinematicIdentity<Tag> current) {
        if (!submitted.IsValid() || !current.IsValid())
            return Result<void>::Failure(MakeError(CinematicErrors::IdentityInvalid));
        if (submitted.stableValue != current.stableValue)
            return Result<void>::Failure(MakeError(CinematicErrors::IdentityUnknown));
        if (submitted.generation != current.generation)
            return Result<void>::Failure(MakeError(CinematicErrors::IdentityStale));
        return Result<void>::Success();
    }

    /**
     * @brief Advances an identity generation without changing its durable value or wrapping.
     * @param current Current valid identity retired by its owner.
     * @return Replacement identity, IdentityInvalid, or GenerationExhausted.
     */
    template <typename Tag>
    [[nodiscard]] Result<CinematicIdentity<Tag>> AdvanceCinematicIdentityGeneration(const CinematicIdentity<Tag> current) {
        if (!current.IsValid())
            return Result<CinematicIdentity<Tag>>::Failure(MakeError(CinematicErrors::IdentityInvalid));
        if (current.generation == std::numeric_limits<std::uint32_t>::max())
            return Result<CinematicIdentity<Tag>>::Failure(MakeError(CinematicErrors::GenerationExhausted));
        return MakeCinematicIdentity<Tag>(current.stableValue, current.generation + 1U);
    }

    /** @brief Inert identity group used by playback and authoring test harnesses. */
    struct CinematicIdentityComposition final {
        SequenceId sequence{};       /**< Sequence identity. */
        TrackId track{};             /**< Track identity. */
        KeyframeId keyframe{};       /**< Keyframe identity. */
        PropertyBindingId binding{}; /**< Property-binding identity. */

        /** @brief Reports whether this is the explicit null composition. @return True when every identity is invalid. */
        [[nodiscard]] constexpr bool IsNull() const noexcept {
            return !sequence.IsValid() && !track.IsValid() && !keyframe.IsValid() && !binding.IsValid();
        }

        /** @brief Reports whether every identity is usable. @return True for a complete composition. */
        [[nodiscard]] constexpr bool IsComplete() const noexcept {
            return sequence.IsValid() && track.IsValid() && keyframe.IsValid() && binding.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const CinematicIdentityComposition &) const noexcept = default;
    };

    /** @brief Returns an inert explicit null composition. @return A composition containing no usable identity. */
    [[nodiscard]] constexpr CinematicIdentityComposition NullCinematicIdentityComposition() noexcept {
        return {};
    }

    /**
     * @brief Creates a repeatable complete composition for playback and authoring harnesses.
     * @param firstStableValue First of four consecutive durable values.
     * @param generation Shared non-zero generation.
     * @return Complete composition or IdentityInvalid when the range cannot represent four identities.
     * @note Construction is inert metadata; it performs no registration or ambient mutation.
     */
    [[nodiscard]] Result<CinematicIdentityComposition> MakeDeterministicCinematicIdentityComposition(std::uint64_t firstStableValue,
                                                                                                     std::uint32_t generation);
}  // namespace Horo::Cinematic
