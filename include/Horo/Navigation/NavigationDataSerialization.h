#pragma once

/**
 * @file NavigationDataSerialization.h
 * @brief Bounded canonical serialization and explicit migration for authored navigation records.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Navigation/NavigationIdentity.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Navigation {
    struct NavigationAuthoredRecordIdentityTag;
    struct NavigationAuthoredRecordTypeIdentityTag;

    /** @brief Stable identity of one authored navigation record. */
    using NavigationAuthoredRecordId = NavigationIdentity<NavigationAuthoredRecordIdentityTag>;
    /** @brief Stable type identity of one authored or generated navigation record. */
    using NavigationAuthoredRecordTypeId = NavigationIdentity<NavigationAuthoredRecordTypeIdentityTag>;

    /** @brief Version of the durable navigation source envelope. */
    struct NavigationSourceSchemaVersion final {
        std::uint16_t major{1};
        std::uint16_t minor{1};

        [[nodiscard]] constexpr auto operator<=>(const NavigationSourceSchemaVersion &) const noexcept = default;
    };

    /** @brief Current source schema emitted by the navigation serializer. */
    inline constexpr NavigationSourceSchemaVersion CurrentNavigationSourceSchemaVersion{1, 1};

    /** @brief Policy for authored records whose type is not available to this consumer. */
    enum class NavigationUnknownRecordPolicy : std::uint8_t {
        Reject,
        PreserveOptionalInert,
        Count,
    };

    /** @brief Reason an unknown generated payload was quarantined instead of activated. */
    enum class NavigationGeneratedPayloadQuarantineReason : std::uint8_t {
        UnknownType,
        UnsupportedVersion,
        Count,
    };

    /** @brief Type and inclusive version range understood by one navigation consumer. */
    struct NavigationAuthoredRecordSupport final {
        NavigationAuthoredRecordTypeId type;
        NavigationSourceSchemaVersion minimum;
        NavigationSourceSchemaVersion maximum;

        [[nodiscard]] constexpr auto operator<=>(const NavigationAuthoredRecordSupport &) const noexcept = default;
    };

    /** @brief Hard parser and serializer bounds; callers may lower but never exceed compiled ceilings. */
    struct NavigationSerializationLimits final {
        static constexpr std::size_t MaximumEnvelopeBytes = 4U * 1024U * 1024U;
        static constexpr std::size_t MaximumAuthoredRecords = 4'096;
        static constexpr std::size_t MaximumGeneratedPayloads = 1'024;
        static constexpr std::size_t MaximumRecordPayloadBytes = 1U * 1024U * 1024U;
        static constexpr std::size_t MaximumGeneratedPayloadBytes = 4U * 1024U * 1024U;
        static constexpr std::size_t MaximumTotalPayloadBytes = 4U * 1024U * 1024U;

        std::size_t maximumEnvelopeBytes{MaximumEnvelopeBytes};
        std::size_t maximumAuthoredRecords{MaximumAuthoredRecords};
        std::size_t maximumGeneratedPayloads{MaximumGeneratedPayloads};
        std::size_t maximumRecordPayloadBytes{MaximumRecordPayloadBytes};
        std::size_t maximumGeneratedPayloadBytes{MaximumGeneratedPayloadBytes};
        std::size_t maximumTotalPayloadBytes{MaximumTotalPayloadBytes};

        [[nodiscard]] constexpr auto operator<=>(const NavigationSerializationLimits &) const noexcept = default;
    };

    /** @brief Caller-owned validation policy for one source decode or authored capture. */
    struct NavigationSourceLoadContext final {
        NavigationUnknownRecordPolicy unknownPolicy{NavigationUnknownRecordPolicy::Reject};
        std::span<const NavigationAuthoredRecordSupport> supportedRecords;
        std::span<const NavigationAuthoredRecordSupport> supportedGeneratedPayloads;
        NavigationSerializationLimits limits{};
    };

    /**
     * @brief One durable authored record with a stable identity and opaque payload bytes.
     *
     * `opaque` is set only when an optional unknown type is intentionally preserved as inert data. It is not a
     * second source of truth and is never serialized as a provider/runtime object.
     */
    struct NavigationAuthoredRecord final {
        NavigationAuthoredRecordId id;
        NavigationAuthoredRecordTypeId type;
        NavigationSourceSchemaVersion version;
        bool required{true};
        std::vector<std::byte> payload;
        bool opaque{};

        [[nodiscard]] constexpr bool IsOpaque() const noexcept {
            return opaque;
        }

        [[nodiscard]] constexpr auto Identity() const noexcept {
            return id;
        }

        [[nodiscard]] bool operator==(const NavigationAuthoredRecord &) const = default;
    };

    /** @brief One generated/provider payload carried only as derived data, never as authored Scene truth. */
    struct NavigationGeneratedPayload final {
        NavigationAuthoredRecordTypeId type;
        NavigationSourceSchemaVersion version;
        std::vector<std::byte> payload;

        [[nodiscard]] bool operator==(const NavigationGeneratedPayload &) const = default;
    };

    /** @brief Raw generated payload retained for explicit quarantine/recovery inspection. */
    struct NavigationQuarantinedGeneratedPayload final {
        NavigationGeneratedPayload payload;
        NavigationGeneratedPayloadQuarantineReason reason{NavigationGeneratedPayloadQuarantineReason::UnknownType};

        [[nodiscard]] bool operator==(const NavigationQuarantinedGeneratedPayload &) const = default;
    };

    /**
     * @brief Immutable validated authored navigation records plus non-activatable generated payload quarantine.
     *
     * Construction sorts by stable identity and validates the complete candidate before returning. It never writes a
     * Scene document, live world, registry, or provider. Generated payloads outside the supplied support table are
     * retained only in `QuarantinedGeneratedPayloads()` and are never exposed through `GeneratedPayloads()`.
     */
    class NavigationSourceRecords final {
    public:
        /**
         * @brief Builds a complete authored candidate without generated payloads.
         * @param schemaVersion Source envelope schema version.
         * @param records Authored records to validate and canonically order.
         * @param context Explicit support and bounded-load policy.
         * @return Detached validated records or a typed source error.
         */
        [[nodiscard]] static Result<NavigationSourceRecords> Create(NavigationSourceSchemaVersion schemaVersion,
                                                                    std::vector<NavigationAuthoredRecord> records,
                                                                    const NavigationSourceLoadContext &context = {});

        /**
         * @brief Builds a complete authored candidate and quarantines unsupported generated payloads.
         * @param schemaVersion Source envelope schema version.
         * @param records Authored records to validate and canonically order.
         * @param generatedPayloads Derived payloads to activate or quarantine.
         * @param context Explicit support and bounded-load policy.
         * @return Detached validated records or a typed source error.
         */
        [[nodiscard]] static Result<NavigationSourceRecords> Create(NavigationSourceSchemaVersion schemaVersion,
                                                                    std::vector<NavigationAuthoredRecord> records,
                                                                    std::vector<NavigationGeneratedPayload> generatedPayloads,
                                                                    const NavigationSourceLoadContext &context = {});

        /** @brief Returns the exact source envelope schema version; no migration is performed implicitly. @return Source schema version. */
        [[nodiscard]] NavigationSourceSchemaVersion SchemaVersion() const noexcept;
        /** @brief Alias for callers that use version terminology. @return Source schema version. */
        [[nodiscard]] NavigationSourceSchemaVersion Version() const noexcept;
        /** @brief Returns authored records in canonical stable-identity order. @return Borrowed immutable records. */
        [[nodiscard]] std::span<const NavigationAuthoredRecord> Records() const noexcept;
        /** @brief Returns only generated payloads supported by the supplied load context. @return Borrowed activatable payloads. */
        [[nodiscard]] std::span<const NavigationGeneratedPayload> GeneratedPayloads() const noexcept;
        /** @brief Returns generated payloads that were retained but quarantined from activation. @return Borrowed quarantined payloads. */
        [[nodiscard]] std::span<const NavigationQuarantinedGeneratedPayload> QuarantinedGeneratedPayloads() const noexcept;
        /** @brief Reports whether any derived payload was quarantined. @return True when quarantine is non-empty. */
        [[nodiscard]] bool HasQuarantinedGeneratedPayloads() const noexcept;

    private:
        friend Result<NavigationSourceRecords> DeserializeNavigationSourceRecords(std::span<const std::byte> bytes,
                                                                                  const NavigationSourceLoadContext &context);
        friend Result<NavigationSourceRecords> DeserializeNavigationSourceRecords(const std::vector<std::byte> &bytes,
                                                                                  const NavigationSourceLoadContext &context);

        NavigationSourceRecords(NavigationSourceSchemaVersion schemaVersion, std::vector<NavigationAuthoredRecord> records,
                                std::vector<NavigationGeneratedPayload> generatedPayloads,
                                std::vector<NavigationQuarantinedGeneratedPayload> quarantinedGeneratedPayloads) noexcept;

        NavigationSourceSchemaVersion schemaVersion_;
        std::vector<NavigationAuthoredRecord> records_;
        std::vector<NavigationGeneratedPayload> generatedPayloads_;
        std::vector<NavigationQuarantinedGeneratedPayload> quarantinedGeneratedPayloads_;
    };

    /**
     * @brief One explicit authored-schema transformation edge.
     *
     * The function receives canonical records only. Generated data is intentionally not an input to migration because
     * generated/provider payloads are invalidated or quarantined rather than source-migrated.
     */
    using NavigationSourceMigrationFunction =
        Result<std::vector<NavigationAuthoredRecord>> (*)(std::span<const NavigationAuthoredRecord> records);

    /** @brief Explicit, exact schema migration edge; no edge may be inferred from version ordering. */
    struct NavigationSourceMigrationStep final {
        NavigationSourceSchemaVersion from;
        NavigationSourceSchemaVersion to;
        NavigationSourceMigrationFunction upgrade{};
    };

    /**
     * @brief Applies an explicitly supplied migration chain to an immutable source candidate.
     * @param source Parsed source records; parsing never invokes this function implicitly.
     * @param targetVersion Exact desired schema version.
     * @param steps Explicit version-to-version transformations, supplied by the owning schema.
     * @param context Support and bounded-load policy for the migrated candidate.
     * @return Migrated candidate or a missing/ambiguous/invalid migration error.
     */
    [[nodiscard]] Result<NavigationSourceRecords> MigrateNavigationSourceRecords(const NavigationSourceRecords &source,
                                                                                 NavigationSourceSchemaVersion targetVersion,
                                                                                 std::span<const NavigationSourceMigrationStep> steps,
                                                                                 const NavigationSourceLoadContext &context = {});

    /** @brief Descriptive alias for the explicit migration operation. */
    [[nodiscard]] Result<NavigationSourceRecords> UpgradeNavigationSourceRecords(const NavigationSourceRecords &source,
                                                                                 NavigationSourceSchemaVersion targetVersion,
                                                                                 std::span<const NavigationSourceMigrationStep> steps,
                                                                                 const NavigationSourceLoadContext &context = {});

    /**
     * @brief Serializes a validated source candidate into one canonical little-endian envelope.
     * @param source Complete immutable authored/derived candidate.
     * @param limits Bounds applied before output allocation.
     * @return Owned canonical bytes or a typed capacity/serialization failure.
     */
    [[nodiscard]] Result<std::vector<std::byte>> SerializeNavigationSourceRecords(const NavigationSourceRecords &source,
                                                                                  const NavigationSerializationLimits &limits = {});

    /**
     * @brief Parses one complete canonical envelope without mutating a live world or Scene document.
     * @param bytes Complete bounded envelope bytes.
     * @param context Explicit supported-record and unknown-payload policy.
     * @return Detached validated source candidate or a typed corruption/version/identity failure.
     */
    [[nodiscard]] Result<NavigationSourceRecords> DeserializeNavigationSourceRecords(std::span<const std::byte> bytes,
                                                                                     const NavigationSourceLoadContext &context = {});

    /** @brief Convenience overload for an owned byte vector. */
    [[nodiscard]] Result<NavigationSourceRecords> DeserializeNavigationSourceRecords(const std::vector<std::byte> &bytes,
                                                                                     const NavigationSourceLoadContext &context = {});
}  // namespace Horo::Navigation
