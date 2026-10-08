#pragma once

/** @file SaveContentRequirements.h
 * @brief Canonical, bounded installed-content declarations owned by runtime-scene composition.
 */
#include "Horo/Assets/AssetArchive.h"
#include "Horo/Runtime/Save/SaveCanonicalCodec.h"
#include "Horo/Runtime/Save/SaveParticipantRegistry.h"

#include <variant>

namespace Horo::Runtime {
    /** @brief Whether the declared owner's absence must reject world preparation. */
    enum class SaveContentNecessity : std::uint8_t {
        Required,
        Optional
    };

    /** @brief Exact cooked asset identity and compatible encoded-envelope evidence. */
    struct SaveAssetContentRequirement final {
        Assets::AssetId asset;
        Assets::AssetTypeId type;
        Sha256Digest envelopeDigest;
        auto operator<=>(const SaveAssetContentRequirement &) const = default;
    };

    /** @brief Exact distribution chunk identity; installation is established by verified mounted selection. */
    struct SaveChunkContentRequirement final {
        Assets::AssetChunkId chunk;
        Assets::AssetChunkKind kind;
        auto operator<=>(const SaveChunkContentRequirement &) const = default;
    };

    /** @brief Exact native declaration version required by one durable participant. */
    struct SaveModuleContentRequirement final {
        SaveParticipantId module;
        std::uint32_t version{};
        auto operator<=>(const SaveModuleContentRequirement &) const = default;
    };

    /** @brief Schema-owned dependency; owner maps directly to archive manifest persistence authority. */
    struct SaveContentRequirement final {
        SaveParticipantId owner;
        SaveContentNecessity necessity{SaveContentNecessity::Required};
        std::variant<SaveAssetContentRequirement, SaveChunkContentRequirement, SaveModuleContentRequirement> content;
        auto operator<=>(const SaveContentRequirement &) const = default;
    };

    /** @brief Qualified hard ceiling for one required content-declaration record. */
    inline constexpr std::size_t MaximumSaveContentRequirements = 1'024;
    /** @brief Returns the reserved built-in semantic owner of schema version 1. @return Valid stable participant identity. */
    [[nodiscard]] SaveParticipantId SaveContentRequirementsParticipant();
    /** @brief Returns the reserved record identity for the built-in requirement declaration. @return Valid stable schema record. */
    [[nodiscard]] SaveRecordId SaveContentRequirementsRecord();
    /** @brief Validates bounded identities and canonical unique order without provider/module callbacks.
     * @param requirements Canonical owner/kind/identity ordered declarations.
     * @return Success or typed schema, duplicate, order or capacity error.
     */
    [[nodiscard]] Result<void> ValidateSaveContentRequirements(std::span<const SaveContentRequirement> requirements);
    /** @brief Encodes required participant schema version 1 using the existing canonical scalar/sequence codec.
     * @param requirements Valid canonical ordered declarations.
     * @return Bounded immutable encoded declaration, or typed validation/allocation failure.
     */
    [[nodiscard]] Result<CanonicalEncodedValue> EncodeSaveContentRequirements(std::span<const SaveContentRequirement> requirements);
    /** @brief Decodes schema version 1 before any world or project source is invoked.
     * @param bytes Exact decoded record bytes from the validated archive reader.
     * @return Owned canonical declarations, or typed malformed/newer-schema/order/capacity failure.
     */
    [[nodiscard]] Result<std::vector<SaveContentRequirement>> DecodeSaveContentRequirements(std::span<const std::byte> bytes);
}  // namespace Horo::Runtime
