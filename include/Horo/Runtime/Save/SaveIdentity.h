#pragma once

/**
 * @file SaveIdentity.h
 * @brief Backend-neutral persistent save identities and independent version value types.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace Horo::Runtime {
    namespace SaveIdentityDetail {
        /** @brief Canonical persistent bytes shared by every opaque save identity. */
        using Bytes = std::array<std::uint8_t, 16>;

        /** @brief Parses canonical lowercase UUID text. */
        [[nodiscard]] Result<Bytes> ParseUuid(std::string_view text);
        /** @brief Validates persistent identity bytes and rejects the reserved all-zero value. */
        [[nodiscard]] Result<Bytes> ValidateUuid(Bytes bytes);
        /** @brief Formats canonical lowercase UUID text. */
        [[nodiscard]] std::string FormatUuid(const Bytes &bytes);
        /** @brief Computes the stable 64-bit FNV-1a hash of canonical identity bytes. */
        [[nodiscard]] std::uint64_t HashUuid(const Bytes &bytes) noexcept;
    }  // namespace SaveIdentityDetail

    /**
     * @brief Strong non-zero opaque 128-bit persistent identity.
     *
     * Tags define semantic ownership. The canonical representation is fixed UUID bytes/text;
     * paths, names, addresses, native handles, RTTI, clocks, and compiler layouts are excluded.
     */
    template <typename Tag> class PersistentSaveIdentity final {
    public:
        /** @brief Constructs the reserved invalid identity for optional/default storage. */
        PersistentSaveIdentity() = default;

        /** @brief Parses canonical lowercase UUID text. @param text Text to parse.
         * @return A non-zero typed identity or a stable save identity error.
         */
        [[nodiscard]] static Result<PersistentSaveIdentity> Parse(const std::string_view text) {
            auto parsed = SaveIdentityDetail::ParseUuid(text);
            if (parsed.HasError())
                return Result<PersistentSaveIdentity>::Failure(parsed.ErrorValue());
            return Result<PersistentSaveIdentity>::Success(PersistentSaveIdentity{parsed.Value()});
        }

        /** @brief Constructs an identity from canonical persistent bytes. @param bytes UUID bytes.
         * @return A non-zero typed identity or SaveErrors::IdentityInvalid.
         */
        [[nodiscard]] static Result<PersistentSaveIdentity> FromBytes(SaveIdentityDetail::Bytes bytes) {
            auto validated = SaveIdentityDetail::ValidateUuid(bytes);
            if (validated.HasError())
                return Result<PersistentSaveIdentity>::Failure(validated.ErrorValue());
            return Result<PersistentSaveIdentity>::Success(PersistentSaveIdentity{validated.Value()});
        }

        /** @brief Reports whether the identity is non-zero. @return True for a usable persistent identity. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns canonical UUID bytes. @return Borrowed immutable persistent bytes. */
        [[nodiscard]] const SaveIdentityDetail::Bytes &Bytes() const noexcept;
        /** @brief Formats canonical lowercase UUID text. @return Newly allocated canonical text. */
        [[nodiscard]] std::string ToString() const;

        [[nodiscard]] constexpr auto operator<=>(const PersistentSaveIdentity &) const noexcept = default;

    private:
        explicit constexpr PersistentSaveIdentity(SaveIdentityDetail::Bytes bytes) noexcept : bytes_(bytes) {}

        SaveIdentityDetail::Bytes bytes_{};
    };

    /** @brief Hash adapter for strongly typed persistent save identities. */
    template <typename Tag> struct PersistentSaveIdentityHash final {
        /** @brief Hashes canonical identity bytes. @param value Identity to hash.
         * @return Stable FNV-1a hash independent of process and standard-library implementation.
         */
        [[nodiscard]] std::size_t operator()(const PersistentSaveIdentity<Tag> &value) const noexcept {
            return static_cast<std::size_t>(SaveIdentityDetail::HashUuid(value.Bytes()));
        }
    };

    struct ProductStorageIdentityTag;
    struct EnvironmentStorageIdentityTag;
    struct LocalUserStorageIdentityTag;
    struct GameProfileIdentityTag;
    struct ServerStorageOwnerIdentityTag;
    struct SaveGameSlotIdentityTag;
    struct SlotGenerationIdentityTag;
    struct SaveRecordIdentityTag;
    struct CapturedStateIdentityTag;
    struct SaveAssetIdentityTag;
    struct PersistentEntityIdentityTag;

    /** @brief Product configuration identity, never derived from executable name or location. */
    using ProductStorageId = PersistentSaveIdentity<ProductStorageIdentityTag>;
    /** @brief Explicit production, development, test, or PIE storage partition identity. */
    using EnvironmentStorageId = PersistentSaveIdentity<EnvironmentStorageIdentityTag>;
    /** @brief Product-local opaque user identity containing no platform display or account name. */
    using LocalUserStorageId = PersistentSaveIdentity<LocalUserStorageIdentityTag>;
    /** @brief Product-owned game profile identity, distinct from user and display metadata. */
    using GameProfileId = PersistentSaveIdentity<GameProfileIdentityTag>;
    /** @brief Dedicated-server world/tenant storage owner identity. */
    using ServerStorageOwnerId = PersistentSaveIdentity<ServerStorageOwnerIdentityTag>;
    /** @brief Logical slot identity that remains stable across successful overwrites. */
    using SaveGameSlotId = PersistentSaveIdentity<SaveGameSlotIdentityTag>;
    /** @brief Identity of one intended durable publication, distinct from its logical slot. */
    using SlotGenerationId = PersistentSaveIdentity<SlotGenerationIdentityTag>;
    /** @brief Stable identity of one canonical save record. */
    using SaveRecordId = PersistentSaveIdentity<SaveRecordIdentityTag>;
    /** @brief Stable identity assigned to one detached captured-state value. */
    using CapturedStateId = PersistentSaveIdentity<CapturedStateIdentityTag>;
    /** @brief Stable Asset Registry identity shared by save references and gameplay fixups. */
    using SaveAssetId = PersistentSaveIdentity<SaveAssetIdentityTag>;
    /** @brief Stable durable entity identity, independent from a runtime Scene domain or ECS slot. */
    using PersistentEntityId = PersistentSaveIdentity<PersistentEntityIdentityTag>;

    /** @brief Maximum canonical participant identity length in bytes. */
    inline constexpr std::size_t MaximumSaveParticipantIdBytes = 96;

    /** @brief Stable lowercase dotted participant identity, independent of RTTI and module addresses. */
    class SaveParticipantId final {
    public:
        SaveParticipantId() = default;
        /** @brief Validates canonical dotted participant text. @param value Text such as `horo.scene.core.v1`.
         * @return Owned typed identity or SaveErrors::ParticipantIdInvalid.
         */
        [[nodiscard]] static Result<SaveParticipantId> Parse(std::string_view value);
        /** @brief Returns owned canonical text. @return Borrowed immutable participant identity. */
        [[nodiscard]] const std::string &Value() const noexcept;
        /** @brief Reports whether the identity is present. @return True for canonical non-empty text. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const SaveParticipantId &) const noexcept = default;

    private:
        explicit SaveParticipantId(std::string value) : value_(std::move(value)) {}

        std::string value_;
    };

    /** @brief Hash adapter for SaveParticipantId containers. */
    struct SaveParticipantIdHash final {
        /** @brief Hashes canonical participant identity bytes. @param value Participant identity.
         * @return Stable FNV-1a hash.
         */
        [[nodiscard]] std::size_t operator()(const SaveParticipantId &value) const noexcept;
    };

    /** @brief Strong non-zero 32-bit version on one independently evolving save version axis. */
    template <typename Tag> class SaveVersion final {
    public:
        SaveVersion() = default;

        /** @brief Validates an encoded version. @param value Non-zero canonical value.
         * @return Typed version or SaveErrors::VersionInvalid.
         */
        [[nodiscard]] static Result<SaveVersion> Create(const std::uint32_t value) {
            if (value == 0)
                return Result<SaveVersion>::Failure(MakeError(SaveErrors::VersionInvalid));
            return Result<SaveVersion>::Success(SaveVersion{value});
        }

        /** @brief Returns the canonical unsigned value. @return Zero only for an invalid default value. */
        [[nodiscard]] constexpr std::uint32_t Value() const noexcept {
            return value_;
        }

        /** @brief Reports whether the value is non-zero. @return True for a usable version. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value_ != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const SaveVersion &) const noexcept = default;

    private:
        explicit constexpr SaveVersion(const std::uint32_t value) noexcept : value_(value) {}

        std::uint32_t value_{};
    };

    struct ArchiveFormatVersionTag;
    struct SaveSchemaVersionTag;
    struct ParticipantSchemaVersionTag;
    struct ProductSaveCompatibilityVersionTag;
    using ArchiveFormatVersion = SaveVersion<ArchiveFormatVersionTag>;
    using SaveSchemaVersion = SaveVersion<SaveSchemaVersionTag>;
    using ParticipantSchemaVersion = SaveVersion<ParticipantSchemaVersionTag>;
    using ProductSaveCompatibilityVersion = SaveVersion<ProductSaveCompatibilityVersionTag>;

    /** @brief Canonical little-endian representation of one save version. */
    using SerializedSaveVersion = std::array<std::uint8_t, sizeof(std::uint32_t)>;

    /** @brief Encodes a version without using native layout. @param version Version to encode.
     * @return Four canonical little-endian bytes; an invalid version encodes as zero.
     */
    template <typename Tag> [[nodiscard]] constexpr SerializedSaveVersion SerializeSaveVersion(const SaveVersion<Tag> version) noexcept {
        const std::uint32_t value = version.Value();
        return {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8U), static_cast<std::uint8_t>(value >> 16U),
                static_cast<std::uint8_t>(value >> 24U)};
    }

    /** @brief Decodes a canonical little-endian version. @param bytes Persistent bytes.
     * @return Typed non-zero version or SaveErrors::VersionInvalid.
     */
    template <typename Tag> [[nodiscard]] Result<SaveVersion<Tag>> DeserializeSaveVersion(const SerializedSaveVersion &bytes) {
        const auto value = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                           (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
        return SaveVersion<Tag>::Create(value);
    }

    /** @brief Rejects a version newer than a reader's supported value. @param input Input version.
     * @param newestReadable Newest version supported by the reader.
     * @return Success when both values are valid and input is not newer; otherwise a typed version error.
     */
    template <typename Tag>
    [[nodiscard]] Result<void> ValidateReadableSaveVersion(const SaveVersion<Tag> input, const SaveVersion<Tag> newestReadable) {
        if (!input.IsValid() || !newestReadable.IsValid())
            return Result<void>::Failure(MakeError(SaveErrors::VersionInvalid));
        if (input > newestReadable)
            return Result<void>::Failure(MakeError(SaveErrors::VersionUnsupportedNewer));
        return Result<void>::Success();
    }

    /** @brief Rejects invalid or duplicate identities before persistence work. @param values Values to validate.
     * @return Success for a unique sequence, including an empty sequence; otherwise a typed identity error.
     */
    template <typename Tag>
    [[nodiscard]] Result<void> ValidateUniqueSaveIdentities(const std::span<const PersistentSaveIdentity<Tag>> values) {
        std::unordered_set<PersistentSaveIdentity<Tag>, PersistentSaveIdentityHash<Tag>> uniqueValues;
        uniqueValues.reserve(values.size());
        for (const auto &value : values) {
            if (!value.IsValid())
                return Result<void>::Failure(MakeError(SaveErrors::IdentityInvalid));
            if (!uniqueValues.insert(value).second)
                return Result<void>::Failure(MakeError(SaveErrors::IdentityDuplicate));
        }
        return Result<void>::Success();
    }

    template <typename Tag> bool PersistentSaveIdentity<Tag>::IsValid() const noexcept {
        for (const std::uint8_t byte : bytes_) {
            if (byte != 0)
                return true;
        }
        return false;
    }

    template <typename Tag> const SaveIdentityDetail::Bytes &PersistentSaveIdentity<Tag>::Bytes() const noexcept {
        return bytes_;
    }

    template <typename Tag> std::string PersistentSaveIdentity<Tag>::ToString() const {
        return SaveIdentityDetail::FormatUuid(bytes_);
    }
}  // namespace Horo::Runtime
