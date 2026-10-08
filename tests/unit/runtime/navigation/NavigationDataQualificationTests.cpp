#include "AllocationProbe.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "navigation/NavigationDataQualificationCorpus.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <iostream>
#include <new>

namespace Horo::Navigation {
    namespace {
        using TestSupport::Id;
        using TestSupport::NavigationDataQualificationCorpus;
        using TestSupport::RequireError;
        namespace Probe = Tests::AllocationProbe;

        /** @brief Observe only the actual synchronous production parser, outside corpus/assertion/report allocation. */
        [[nodiscard]] Result<NavigationDefinition> MeasuredDecode(const NavigationAuthoredRecord &record, Probe::Measurement &measurement) {
            Probe::ScopedMeasurement window;
            auto result = DecodeNavigationDefinitionRecord(record);
            measurement = window.Snapshot();
            return result;
        }

        /** @brief Report only measured parser requests with the configured workload and platform. */
        void Report(const std::string_view workload, const std::size_t bytes, const Probe::Measurement measurement) {
            std::cout << "navigation-data-qualification build=" << HORO_QUALIFICATION_BUILD_TYPE
                      << " platform=" << HORO_QUALIFICATION_PLATFORM << " workload=" << workload << " encoded_bytes=" << bytes
                      << " cpp_requests=" << measurement.requests << " cpp_requested_bytes=" << measurement.requestedBytes
                      << " cpp_largest_request=" << measurement.largestRequest << '\n';
        }

        /** @brief Recompute the genuine portable checksum after a corpus framing mutation, so validation reaches the hostile field. */
        void RefreshChecksum(std::vector<std::byte> &bytes) {
            REQUIRE(bytes.size() >= 32);
            const auto digest = ComputeSha256(std::span<const std::byte>{bytes.data(), bytes.size() - 32});
            const auto encoded = std::as_bytes(std::span{digest.bytes});
            std::copy(encoded.begin(), encoded.end(), bytes.end() - static_cast<std::ptrdiff_t>(encoded.size()));
        }
    }  // namespace

    TEST_CASE("Navigation qualification measurement isolates ordinary aligned and nested allocation windows",
              "[unit][navigation][data_qualification][allocation_probe]") {
        Probe::Measurement outer;
        Probe::Measurement inner;
        {
            Probe::ScopedMeasurement window;
            void *ordinary = ::operator new(64);
            {
                Probe::ScopedMeasurement nested;
                void *aligned = ::operator new(128, std::align_val_t{64});
                inner = nested.Snapshot();
                ::operator delete(aligned, std::align_val_t{64});
            }
            outer = window.Snapshot();
            ::operator delete(ordinary);
        }
        CHECK(outer.requests == 1);
        CHECK(outer.requestedBytes == 64);
        CHECK(outer.largestRequest == 64);
        CHECK(inner.requests == 1);
        CHECK(inner.requestedBytes == 128);
        CHECK(inner.largestRequest == 128);
    }

    TEST_CASE("Portable NAVDEF01 corpus pins canonical bytes and captures the same multiprofile geometry model",
              "[unit][navigation][data_qualification][portable]") {
        const NavigationDataQualificationCorpus corpus;
        const auto record = corpus.Record();
        // Independently assembled little-endian NAVDEF01 1.0 fixture, including its exact UTF-8 text and binary32 values.
        CHECK(record.payload.size() == 201);
        CHECK(FormatSha256(ComputeSha256(record.payload)) == "sha256:f222d1ed83d2e016d04ea81c238cbc964f00b2c570e51e041bd5e13ef93d2586");
        REQUIRE(corpus.Record(true) == record);
        const std::array support{NavigationDefinitionRecordSupport()};
        const NavigationSourceLoadContext context{.supportedRecords = support};
        auto source = NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {record}, context);
        REQUIRE(source.HasValue());
        const auto encoded = SerializeNavigationSourceRecords(source.Value());
        REQUIRE(encoded.HasValue());
        const auto restored = DeserializeNavigationSourceRecords(encoded.Value(), context);
        REQUIRE(restored.HasValue());
        REQUIRE(restored.Value().Records().size() == 1);
        Probe::Measurement measurement;
        const auto definition = MeasuredDecode(restored.Value().Records().front(), measurement);
        REQUIRE(definition.HasValue());
        REQUIRE(definition.Value().Profiles().size() == 2);
        const auto input = corpus.Capture(definition.Value());
        const auto reversed = corpus.Capture(definition.Value(), true);
        CHECK(input.Fingerprint() == reversed.Fingerprint());
        REQUIRE(input.Partitions().size() == 2);
        for (std::size_t index = 0; index < input.Partitions().size(); ++index) {
            CHECK(input.Partitions()[index].profile == definition.Value().Profiles()[index].id);
            CHECK(input.Partitions()[index].surface == Id<SurfaceId>(1));
            CHECK(input.Partitions()[index].triangleCount == corpus.collision.triangles.size());
        }
        Report("valid-multiprofile", record.payload.size(), measurement);
    }

    TEST_CASE("Portable authored corpus rejects every truncation and hostile count with stable bounded parser diagnostics",
              "[unit][navigation][data_qualification][malformed][allocation]") {
        const NavigationDataQualificationCorpus corpus;
        const auto valid = corpus.Record();
        REQUIRE(valid.payload.size() == 201);
        for (std::size_t prefix = 0; prefix < valid.payload.size(); ++prefix) {
            auto malformed = valid;
            malformed.payload.resize(prefix);
            Probe::Measurement measurement;
            const auto rejected = MeasuredDecode(malformed, measurement);
            INFO("prefix=" << prefix);
            RequireError(rejected, NavigationErrors::SourceEnvelopeInvalid);
            CHECK(measurement.largestRequest <= 4096);
            CHECK(measurement.requestedBytes <= 16U * 1024U);
        }
        auto hostile = valid;
        std::fill(hostile.payload.begin() + 20, hostile.payload.begin() + 24, std::byte{0xff});
        Probe::Measurement measurement;
        const auto rejected = MeasuredDecode(hostile, measurement);
        RequireError(rejected, NavigationErrors::SourceEnvelopeInvalid);
        CHECK(measurement.requests < 16);
        CHECK(measurement.largestRequest <= 4096);
        CHECK(measurement.requestedBytes <= 16U * 1024U);
        CHECK(corpus.Record() == valid);
        Report("hostile-profile-count", hostile.payload.size(), measurement);
    }

    TEST_CASE("Portable authored corpus never converts version skew or opaque records into active definition policy",
              "[unit][navigation][data_qualification][version_skew]") {
        const NavigationDataQualificationCorpus corpus;
        auto record = corpus.Record();
        const ErrorCodeDescriptor *expected = &NavigationErrors::SourceUnknownAuthoredRecord;
        SECTION("future payload minor") {
            record.version.minor += 1;
            expected = &NavigationErrors::SourceUnsupportedVersion;
        }
        SECTION("legacy payload major") {
            record.version.major = 0;
            expected = &NavigationErrors::SourceUnsupportedVersion;
        }
        SECTION("optional inert payload") {
            record.required = false;
            record.opaque = true;
        }
        SECTION("foreign authored record") {
            record.type = Id<NavigationAuthoredRecordTypeId>(999);
        }
        Probe::Measurement measurement;
        const auto rejected = MeasuredDecode(record, measurement);
        RequireError(rejected, *expected);
        CHECK(measurement.largestRequest <= 4096);
        CHECK(measurement.requestedBytes <= 16U * 1024U);
    }

    TEST_CASE("Portable HNAV corpus enforces lowered payload and hostile framing bounds before detached publication",
              "[unit][navigation][data_qualification][envelope][allocation]") {
        const NavigationDataQualificationCorpus corpus;
        const std::array support{NavigationDefinitionRecordSupport()};
        const NavigationSourceLoadContext context{.supportedRecords = support};
        const auto source = NavigationSourceRecords::Create(CurrentNavigationSourceSchemaVersion, {corpus.Record()}, context);
        REQUIRE(source.HasValue());
        const auto encoded = SerializeNavigationSourceRecords(source.Value());
        REQUIRE(encoded.HasValue());
        auto malformed = encoded.Value();
        REQUIRE(malformed.size() >= 93);  // All mutated fields precede the final 32-byte checksum.
        auto limits = context;
        const ErrorCodeDescriptor *expected = &NavigationErrors::SourceSerializationCapacityExceeded;
        SECTION("lowered semantic record bytes") {
            limits.limits.maximumRecordPayloadBytes = 200;
        }
        SECTION("hostile authored count") {
            std::fill(malformed.begin() + 12, malformed.begin() + 16, std::byte{0xff});
            RefreshChecksum(malformed);
        }
        SECTION("hostile encoded payload size") {
            std::fill(malformed.begin() + 50, malformed.begin() + 54, std::byte{0xff});
            RefreshChecksum(malformed);
        }
        SECTION("future source schema") {
            malformed[4] = std::byte{2};
            RefreshChecksum(malformed);
            expected = &NavigationErrors::SourceUnsupportedVersion;
        }
        SECTION("corrupt source bytes") {
            malformed[60] ^= std::byte{1};
            expected = &NavigationErrors::SourceChecksumMismatch;
        }
        Probe::Measurement measurement;
        auto rejected = [&] {
            Probe::ScopedMeasurement window;
            auto result = DeserializeNavigationSourceRecords(malformed, limits);
            measurement = window.Snapshot();
            return result;
        }();
        RequireError(rejected, *expected);
        CHECK(measurement.largestRequest <= 4096);
        CHECK(measurement.requestedBytes <= 16U * 1024U);
        CHECK(SerializeNavigationSourceRecords(source.Value()).Value() == encoded.Value());
        Report("rejected-HNAV", malformed.size(), measurement);
    }

    TEST_CASE("Every observed definition allocation failure preserves stable owner capacity diagnostics without partial authored policy",
              "[unit][navigation][data_qualification][allocation_failure]") {
        const NavigationDataQualificationCorpus corpus;
        const auto record = corpus.Record();
        Probe::Measurement successful;
        const auto valid = MeasuredDecode(record, successful);
        REQUIRE(valid.HasValue());
        REQUIRE(successful.requests > 0);
        for (std::size_t index = 0; index < successful.requests; ++index) {
            const auto rejected = [&] {
                Probe::ScopedFailure failure{index};
                return DecodeNavigationDefinitionRecord(record);
            }();
            INFO("allocation request=" << index);
            REQUIRE(rejected.HasError());
            if (rejected.ErrorValue().code.Value() == NavigationErrors::SourceSerializationCapacityExceeded.code.Value())
                RequireError(rejected, NavigationErrors::SourceSerializationCapacityExceeded);
            else
                RequireError(rejected, NavigationErrors::CapacityExceeded);  // Registry-owned failure remains untranslated.
            const auto repeated = [&] {
                Probe::ScopedFailure failure{index};
                return DecodeNavigationDefinitionRecord(record);
            }();
            REQUIRE(repeated.HasError());
            CHECK(repeated.ErrorValue().domain.Value() == rejected.ErrorValue().domain.Value());
            CHECK(repeated.ErrorValue().code.Value() == rejected.ErrorValue().code.Value());
            CHECK(record == corpus.Record());
        }
        CHECK(EncodeNavigationDefinitionRecord(valid.Value(), record.id).Value() == record);
    }
}  // namespace Horo::Navigation
