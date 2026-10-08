#pragma once

/**
 * @file SaveReference.h
 * @brief Stable path-independent durable save references and flat reconciliation results.
 */

#include "Horo/Runtime/Save/SaveArchiveMetadata.h"
#include "Horo/Runtime/Save/SaveCanonicalCodec.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <variant>

namespace Horo::Runtime {
    struct PrefabInstanceIdentityTag;
    /** @brief Stable identity of one durable prefab occurrence. */
    using SavePrefabInstanceId = PersistentSaveIdentity<PrefabInstanceIdentityTag>;

    /** @brief Durable one-byte tags; numeric values are part of the persistent wire contract. */
    enum class SaveReferenceWireTag : std::uint8_t {
        Null = 0x00,
        Asset = 0x01,
        Scene = 0x02,
        Entity = 0x03,
        Prefab = 0x04,
        Participant = 0x05,
        Record = 0x06,
    };

    /** @brief Stable asset target. */
    struct SaveAssetReference final {
        SaveAssetId asset;
        auto operator<=>(const SaveAssetReference &) const noexcept = default;
    };

    /** @brief Stable authored scene target inside a logical world. */
    struct SaveSceneReference final {
        SaveWorldId world;
        SaveBaseSceneId scene;
        auto operator<=>(const SaveSceneReference &) const noexcept = default;
    };

    /** @brief Stable entity target inside a logical world. */
    struct SaveEntityReference final {
        SaveWorldId world;
        PersistentEntityId entity;
        auto operator<=>(const SaveEntityReference &) const noexcept = default;
    };

    /** @brief Stable provenance for an entity instantiated from a prefab asset. */
    struct SavePrefabProvenance final {
        SaveAssetId prefab;
        SavePrefabInstanceId instance;
        auto operator<=>(const SavePrefabProvenance &) const noexcept = default;
    };

    /** @brief Stable participant target. */
    struct SaveParticipantReference final {
        SaveParticipantId participant;
        auto operator<=>(const SaveParticipantReference &) const noexcept = default;
    };

    /** @brief Stable cross-record target decoded independently from its referenced record. */
    struct SaveRecordReference final {
        SaveParticipantId participant;
        SaveRecordId record;
        auto operator<=>(const SaveRecordReference &) const noexcept = default;
    };

    /** @brief Closed durable reference forms; monostate is the canonical null reference. */
    using SaveReferenceTarget = std::variant<std::monostate, SaveAssetReference, SaveSceneReference, SaveEntityReference,
                                             SavePrefabProvenance, SaveParticipantReference, SaveRecordReference>;

    /** @brief Flat post-decode reconciliation state that never recursively owns referenced records. */
    enum class SaveReferenceDisposition : std::uint8_t {
        Resolved = 0,
        Missing = 1,
        Remapped = 2,
        Deferred = 3
    };

    /** @brief Reconciliation evidence retaining the original target for missing/deferred diagnostics. */
    struct SaveReferenceResolution final {
        SaveReferenceTarget original; /**< Decoded target providing required reconciliation context. */
        SaveReferenceDisposition disposition{SaveReferenceDisposition::Missing}; /**< Explicit reconciliation outcome. */
        std::optional<SaveReferenceTarget> replacement;                          /**< Changed same-kind target, only for Remapped. */
    };

    /** @brief Validates a durable reference without resolving external state. @param target Candidate target. @return Success or typed
     * validation error. */
    [[nodiscard]] Result<void> ValidateSaveReference(const SaveReferenceTarget &target);

    /** @brief Canonically encodes one flat durable reference. @param target Valid stable target. @param limits Codec bounds.
     * @return Sealed canonical value or typed validation, quota, or allocation failure. */
    [[nodiscard]] Result<CanonicalEncodedValue> EncodeSaveReference(const SaveReferenceTarget &target,
                                                                    const CanonicalCodecLimits &limits = {});

    /** @brief Decodes one complete durable reference without resolving it. @param bytes Complete encoded value. @param limits Codec
     * bounds. @return Target or typed corruption, quota, configuration, or allocation failure. */
    [[nodiscard]] Result<SaveReferenceTarget> DecodeSaveReference(std::span<const std::byte> bytes,
                                                                  const CanonicalCodecLimits &limits = {});

    /** @brief Validates a separate reconciliation result. @param resolution Candidate result. @return Success or typed validation
     * error. */
    [[nodiscard]] Result<void> ValidateSaveReferenceResolution(const SaveReferenceResolution &resolution);
}  // namespace Horo::Runtime
