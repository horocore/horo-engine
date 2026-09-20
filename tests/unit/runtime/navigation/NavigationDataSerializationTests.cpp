#include "Horo/Foundation/Sha256.h"
#include "Horo/Navigation/NavigationDataSerialization.h"
#include "navigation/NavigationTestAssertions.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        using RecordId = NavigationAuthoredRecordId;
        using TypeId = NavigationAuthoredRecordTypeId;

        [[nodiscard]] RecordId RecordIdentity(const std::uint64_t value) {
            return RecordId::Create(value).Value();
        }

        [[nodiscard]] TypeId RecordType(const std::uint64_t value) {
            return TypeId::Create(value).Value();
        }

        [[nodiscard]] NavigationSourceLoadContext KnownContext(
            const NavigationUnknownRecordPolicy policy = NavigationUnknownRecordPolicy::Reject) {
            static const std::array<NavigationAuthoredRecordSupport, 1> authoredSupport{{
                NavigationAuthoredRecordSupport{.type = RecordType(100), .minimum = {1, 0}, .maximum = {1, 1}},
            }};
            static const std::array<NavigationAuthoredRecordSupport, 1> generatedSupport{{
                NavigationAuthoredRecordSupport{.type = RecordType(200), .minimum = {1, 0}, .maximum = {1, 1}},
            }};
            return {.unknownPolicy = policy, .supportedRecords = authoredSupport, .supportedGeneratedPayloads = generatedSupport};
        }

        [[nodiscard]] NavigationAuthoredRecord Authored(const std::uint64_t id, const std::uint64_t type = 100, const bool required = true,
                                                        const std::uint16_t minor = 0) {
            return {.id = RecordIdentity(id),
                    .type = RecordType(type),
                    .version = {1, minor},
                    .required = required,
                    .payload = {std::byte{static_cast<unsigned char>(id)}, std::byte{0x02}}};
        }

        void RefreshChecksum(std::vector<std::byte> &bytes) {
            const auto digest = ComputeSha256(std::span<const std::byte>{bytes.data(), bytes.size() - 32});
            const auto encoded = std::as_bytes(std::span{digest.bytes});
            std::copy(encoded.begin(), encoded.end(), bytes.end() - static_cast<std::ptrdiff_t>(encoded.size()));
        }

        void WriteLittleEndian(std::vector<std::byte> &bytes, const std::size_t offset, const std::uint64_t value,
                               const std::size_t width) {
            for (std::size_t index = 0; index < width; ++index)
                bytes[offset + index] = std::byte{static_cast<std::uint8_t>(value >> (index * 8U))};
        }

        Result<std::vector<NavigationAuthoredRecord>> UpgradeRecordPayload(const std::span<const NavigationAuthoredRecord> records) {
            std::vector<NavigationAuthoredRecord> upgraded{records.begin(), records.end()};
            for (auto &record : upgraded) {
                record.version = {1, 1};
                record.payload.push_back(std::byte{0x03});
            }
            return Result<std::vector<NavigationAuthoredRecord>>::Success(std::move(upgraded));
        }
    }  // namespace

    TEST_CASE("navigation source records round trip through a bounded canonical envelope", "[unit][navigation][serialization]") {
        const std::array<NavigationAuthoredRecordSupport, 1> support{{
            {.type = RecordType(100), .minimum = {1, 0}, .maximum = {1, 1}},
        }};
        const NavigationSourceLoadContext context{
            .unknownPolicy = NavigationUnknownRecordPolicy::PreserveOptionalInert,
            .supportedRecords = support,
        };
        std::vector<NavigationAuthoredRecord> records{{
            .id = RecordIdentity(7),
            .type = RecordType(100),
            .version = {1, 0},
            .required = true,
            .payload = {std::byte{0x01}, std::byte{0x02}},
        }};

        const auto source = NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, std::move(records), context);
        REQUIRE(source.HasValue());
        const auto encoded = SerializeNavigationSourceRecords(source.Value());
        REQUIRE(encoded.HasValue());
        const auto decoded = DeserializeNavigationSourceRecords(encoded.Value(), context);

        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().Records().size() == 1);
        REQUIRE(decoded.Value().Records().front().id == RecordIdentity(7));
        REQUIRE(decoded.Value().Records().front().payload == std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}});
        REQUIRE(SerializeNavigationSourceRecords(decoded.Value()).Value() == encoded.Value());
    }

    TEST_CASE("navigation serialization canonicalizes stable record order and preserves generated quarantine",
              "[unit][navigation][serialization]") {
        const auto context = KnownContext(NavigationUnknownRecordPolicy::PreserveOptionalInert);
        std::vector<NavigationAuthoredRecord> records{Authored(9), Authored(3)};
        std::vector<NavigationGeneratedPayload> generated{
            {.type = RecordType(999), .version = {1, 0}, .payload = {std::byte{0x09}}},
            {.type = RecordType(200), .version = {1, 0}, .payload = {std::byte{0x08}}},
            {.type = RecordType(200), .version = {2, 0}, .payload = {std::byte{0x07}}},
        };

        const auto source = NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, records, generated, context);
        REQUIRE(source.HasValue());
        REQUIRE(source.Value().Records().front().id == RecordIdentity(3));
        REQUIRE(source.Value().GeneratedPayloads().size() == 1);
        REQUIRE(source.Value().QuarantinedGeneratedPayloads().size() == 2);
        REQUIRE(source.Value().QuarantinedGeneratedPayloads().front().reason ==
                NavigationGeneratedPayloadQuarantineReason::UnsupportedVersion);
        REQUIRE(source.Value().QuarantinedGeneratedPayloads().back().reason == NavigationGeneratedPayloadQuarantineReason::UnknownType);

        const auto reordered =
            NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {Authored(3), Authored(9)}, generated, context);
        REQUIRE(reordered.HasValue());
        const auto encoded = SerializeNavigationSourceRecords(source.Value());
        REQUIRE(encoded.HasValue());
        REQUIRE(SerializeNavigationSourceRecords(reordered.Value()).Value() == encoded.Value());
        const auto decoded = DeserializeNavigationSourceRecords(encoded.Value(), context);
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().Records().front().id == RecordIdentity(3));
        REQUIRE(decoded.Value().GeneratedPayloads().front().type == RecordType(200));
        REQUIRE(decoded.Value().QuarantinedGeneratedPayloads().front().payload.type == RecordType(200));
        REQUIRE(decoded.Value().QuarantinedGeneratedPayloads().back().reason == NavigationGeneratedPayloadQuarantineReason::UnknownType);
        REQUIRE(SerializeNavigationSourceRecords(decoded.Value()).Value() == encoded.Value());
    }

    TEST_CASE("navigation serialization preserves optional unknown authored records as inert opaque data",
              "[unit][navigation][serialization]") {
        const auto context = KnownContext(NavigationUnknownRecordPolicy::PreserveOptionalInert);
        const auto source = NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {Authored(5, 777, false)}, context);
        REQUIRE(source.HasValue());
        REQUIRE(source.Value().Records().front().opaque);

        const auto encoded = SerializeNavigationSourceRecords(source.Value());
        REQUIRE(encoded.HasValue());
        const auto decoded = DeserializeNavigationSourceRecords(encoded.Value(), context);
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().Records().front().opaque);
        REQUIRE(decoded.Value().Records().front().payload == source.Value().Records().front().payload);
        TestSupport::RequireError(DeserializeNavigationSourceRecords(encoded.Value(), KnownContext()),
                                  NavigationErrors::SourceUnknownAuthoredRecord);
        TestSupport::RequireError(NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {Authored(5, 777, true)},
                                                                  KnownContext()),
                                  NavigationErrors::SourceUnknownAuthoredRecord);
    }

    TEST_CASE("navigation serialization rejects invalid support tables and policies", "[unit][navigation][serialization]") {
        const std::array<NavigationAuthoredRecordSupport, 2> duplicateSupport{{
            {.type = RecordType(100), .minimum = {1, 0}, .maximum = {1, 1}},
            {.type = RecordType(100), .minimum = {1, 0}, .maximum = {1, 1}},
        }};
        auto duplicateContext = KnownContext();
        duplicateContext.supportedRecords = duplicateSupport;
        TestSupport::RequireError(NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {Authored(1)}, duplicateContext),
                                  NavigationErrors::SourceEnvelopeInvalid);

        auto invalidPolicy = KnownContext();
        invalidPolicy.unknownPolicy = static_cast<NavigationUnknownRecordPolicy>(0xffU);
        TestSupport::RequireError(NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {Authored(1)}, invalidPolicy),
                                  NavigationErrors::SourceEnvelopeInvalid);

        auto invalidSupport = KnownContext();
        const std::array<NavigationAuthoredRecordSupport, 1> invalidVersion{{
            {.type = RecordType(100), .minimum = {0, 0}, .maximum = {1, 1}},
        }};
        invalidSupport.supportedRecords = invalidVersion;
        TestSupport::RequireError(NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {Authored(1)}, invalidSupport),
                                  NavigationErrors::SourceEnvelopeInvalid);
    }

    TEST_CASE("navigation serialization rejects duplicate identities and malformed bounds", "[unit][navigation][serialization]") {
        const auto context = KnownContext();
        TestSupport::RequireError(NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {Authored(1), Authored(1)},
                                                                  context),
                                  NavigationErrors::SourceDuplicateIdentity);

        auto limited = context;
        limited.limits.maximumRecordPayloadBytes = 1;
        TestSupport::RequireError(NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {Authored(1)}, limited),
                                  NavigationErrors::SourceSerializationCapacityExceeded);

        TestSupport::RequireError(NavigationSourceRecords::Create({1, 2}, {Authored(1)}, context),
                                  NavigationErrors::SourceUnsupportedVersion);
    }

    TEST_CASE("navigation serialization reports corrupt checksums, unsupported envelopes and truncation",
              "[unit][navigation][serialization]") {
        const auto source = NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {Authored(1)}, KnownContext());
        auto encoded = SerializeNavigationSourceRecords(source.Value()).Value();

        encoded[28] ^= std::byte{0x01};
        TestSupport::RequireError(DeserializeNavigationSourceRecords(encoded, KnownContext()), NavigationErrors::SourceChecksumMismatch);

        encoded = SerializeNavigationSourceRecords(source.Value()).Value();
        encoded[4] = std::byte{0x02};
        RefreshChecksum(encoded);
        TestSupport::RequireError(DeserializeNavigationSourceRecords(encoded, KnownContext()), NavigationErrors::SourceUnsupportedVersion);

        encoded = SerializeNavigationSourceRecords(source.Value()).Value();
        encoded.resize(20);
        TestSupport::RequireError(DeserializeNavigationSourceRecords(encoded, KnownContext()), NavigationErrors::SourceEnvelopeInvalid);

        const auto duplicateSource =
            NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {Authored(1), Authored(2)}, KnownContext());
        auto duplicateBytes = SerializeNavigationSourceRecords(duplicateSource.Value()).Value();
        WriteLittleEndian(duplicateBytes, 56, 1, 8);
        RefreshChecksum(duplicateBytes);
        TestSupport::RequireError(DeserializeNavigationSourceRecords(duplicateBytes, KnownContext()),
                                  NavigationErrors::SourceDuplicateIdentity);

        auto invalidIdentity = SerializeNavigationSourceRecords(source.Value()).Value();
        WriteLittleEndian(invalidIdentity, 28, 0, 8);
        RefreshChecksum(invalidIdentity);
        TestSupport::RequireError(DeserializeNavigationSourceRecords(invalidIdentity, KnownContext()),
                                  NavigationErrors::SourceEnvelopeInvalid);

        auto excessiveCount = SerializeNavigationSourceRecords(source.Value()).Value();
        WriteLittleEndian(excessiveCount, 12, NavigationSerializationLimits::MaximumAuthoredRecords + 1, 4);
        RefreshChecksum(excessiveCount);
        TestSupport::RequireError(DeserializeNavigationSourceRecords(excessiveCount, KnownContext()),
                                  NavigationErrors::SourceSerializationCapacityExceeded);

        auto excessivePayload = SerializeNavigationSourceRecords(source.Value()).Value();
        WriteLittleEndian(excessivePayload, 50, NavigationSerializationLimits::MaximumRecordPayloadBytes + 1, 4);
        RefreshChecksum(excessivePayload);
        TestSupport::RequireError(DeserializeNavigationSourceRecords(excessivePayload, KnownContext()),
                                  NavigationErrors::SourceSerializationCapacityExceeded);
    }

    TEST_CASE("navigation source migration is explicit and leaves the source candidate detached", "[unit][navigation][serialization]") {
        const auto context = KnownContext();
        const auto source = NavigationSourceRecords::Create({1, 0}, {Authored(4)}, context);
        REQUIRE(source.HasValue());
        const auto missing = MigrateNavigationSourceRecords(source.Value(), CurrentNavigationSourceSchemaVersion, {}, context);
        TestSupport::RequireError(missing, NavigationErrors::SourceMigrationMissing);
        REQUIRE(source.Value().SchemaVersion() == NavigationSourceSchemaVersion{1, 0});
        REQUIRE(source.Value().Records().front().payload.size() == 2);

        const NavigationSourceMigrationStep step{
            .from = {1, 0},
            .to = {1, 1},
            .upgrade = UpgradeRecordPayload,
        };
        const auto migrated = UpgradeNavigationSourceRecords(source.Value(), CurrentNavigationSourceSchemaVersion, {&step, 1}, context);
        REQUIRE(migrated.HasValue());
        REQUIRE(migrated.Value().SchemaVersion() == CurrentNavigationSourceSchemaVersion);
        REQUIRE(migrated.Value().Records().front().version == NavigationSourceSchemaVersion{1, 1});
        REQUIRE(migrated.Value().Records().front().payload.size() == 3);
        REQUIRE(source.Value().Records().front().version == NavigationSourceSchemaVersion{1, 0});
        REQUIRE(source.Value().Records().front().payload.size() == 2);
    }
}  // namespace Horo::Navigation
