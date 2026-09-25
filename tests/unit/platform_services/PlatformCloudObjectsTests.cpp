#include "Horo/PlatformServices/PlatformCloudObjects.h"
#include "PlatformServicesTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace Horo::PlatformServices {
    using TestSupport::RequireError;

    namespace {
        PlatformSubjectHandle Subject(const std::uint64_t sessionGeneration, const std::byte nonce) {
            PlatformSessionCandidate candidate{.phase = PlatformSessionPhase::Active,
                                               .generation = {sessionGeneration},
                                               .providerGeneration = {7},
                                               .accessRevision = {3}};
            candidate.capabilities.services.fill(PlatformSessionAccessState::Granted);
            PlatformSubjectNonce subject;
            subject.bytes.back() = nonce;
            candidate.subjectNonce = subject;
            auto result = BuildPlatformSessionSnapshot(candidate);
            REQUIRE(result.HasValue());
            return *result.Value().Subject();
        }

        CloudSaveObjectKey Key(const std::byte value) {
            const auto result = CloudSaveObjectKey::Copy(std::array{value});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        ProviderObjectRevision Revision(const std::byte value) {
            const auto result = ProviderObjectRevision::Copy(std::array{value});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        CloudObjectHead Head(const std::byte key, const std::byte revision, const std::uint64_t size,
                             const std::optional<CloudBlobDigest> digest = std::nullopt) {
            return {.key = Key(key), .revision = Revision(revision), .sizeBytes = size, .transportDigest = digest};
        }
    }  // namespace

    TEST_CASE("Cloud object address factories keep keys revisions and cursors opaque and bounded", "[platform-services][cloud][contract]") {
        const auto key = CloudSaveObjectKey::Copy(std::array{std::byte{1}, std::byte{2}});
        REQUIRE(key.HasValue());
        CHECK(key.Value().IsValid());
        CHECK(key.Value().Bytes().size() == 2);
        CHECK_FALSE(std::is_same_v<CloudSaveObjectKey, ProviderObjectRevision>);

        std::vector<std::byte> oversized(MaximumCloudObjectAddressBytes + 1);
        RequireError(CloudSaveObjectKey::Copy(oversized), CloudObjectErrors::InvalidOpaqueValue);
        RequireError(CloudListCursor::Copy({}), CloudObjectErrors::InvalidOpaqueValue);
    }

    TEST_CASE("Cloud object limits reject unbounded provider capability claims", "[platform-services][cloud][validation]") {
        CHECK(ValidateCloudObjectContractLimits({}).HasValue());

        auto limits = CloudObjectContractLimits{};
        limits.maxPageEntries = 0;
        RequireError(ValidateCloudObjectContractLimits(limits), CloudObjectErrors::InvalidLimits);
        limits = {};
        limits.maxObjectBytes = MaximumCloudObjectPayloadBytes + 1;
        RequireError(ValidateCloudObjectContractLimits(limits), CloudObjectErrors::InvalidLimits);
        limits = {};
        limits.maxKeyBytes = MaximumCloudObjectAddressBytes + 1;
        RequireError(ValidateCloudObjectContractLimits(limits), CloudObjectErrors::InvalidLimits);
    }

    TEST_CASE("Cloud list requests and pages are session-partitioned ordered and bounded", "[platform-services][cloud][metadata]") {
        const auto subject = Subject(4, std::byte{1});
        const auto cursor = CloudListCursor::Copy(std::array{std::byte{8}});
        REQUIRE(cursor.HasValue());
        const CloudListRequest request{.subject = subject, .cursor = cursor.Value(), .pageSize = 2};
        REQUIRE(ValidateCloudListRequest(request).HasValue());

        auto page = CloudObjectPage{.subject = subject,
                                    .sessionGeneration = subject.SessionGeneration(),
                                    .objects = {Head(std::byte{1}, std::byte{11}, 0), Head(std::byte{2}, std::byte{12}, 3)},
                                    .hasMore = true};
        page.next = cursor.Value();
        REQUIRE(ValidateCloudObjectPage(page, request, subject).HasValue());

        page.objects.push_back(Head(std::byte{3}, std::byte{13}, 4));
        RequireError(ValidateCloudObjectPage(page, request, subject), CloudObjectErrors::InvalidPage);
        page.objects.pop_back();
        RequireError(ValidateCloudObjectPage(page, request, Subject(5, std::byte{1})), CloudObjectErrors::StaleSession);

        page.objects[1].key = page.objects[0].key;
        RequireError(ValidateCloudObjectPage(page, request, subject), CloudObjectErrors::InvalidPage);
        page.objects[1].key = Key(std::byte{2});
        page.subject = Subject(5, std::byte{2});
        RequireError(ValidateCloudObjectPage(page, request, subject), CloudObjectErrors::StaleSession);
    }

    TEST_CASE("Cloud reads publish only complete matching bytes for the current session", "[platform-services][cloud][read]") {
        const auto subject = Subject(4, std::byte{1});
        const auto key = Key(std::byte{4});
        const std::array bytes{std::byte{9}, std::byte{8}, std::byte{7}};
        const auto owned = CloudBlobOwnedBytes::Copy(bytes);
        REQUIRE(owned.HasValue());
        const auto digest = ComputeSha256(bytes);
        const CloudBlobReadRequest request{.subject = subject, .key = key, .maximumBytes = 3};
        const CloudBlobReadResult result{.subject = subject,
                                         .sessionGeneration = subject.SessionGeneration(),
                                         .head = {.key = key,
                                                  .revision = Revision(std::byte{2}),
                                                  .sizeBytes = 3,
                                                  .transportDigest = digest},
                                         .bytes = owned.Value()};
        REQUIRE(ValidateCloudBlobReadCompletion(result, request, subject).HasValue());

        auto staleResult = result;
        staleResult.subject = Subject(5, std::byte{2});
        staleResult.sessionGeneration = staleResult.subject.SessionGeneration();
        RequireError(ValidateCloudBlobReadCompletion(staleResult, request, subject), CloudObjectErrors::StaleSession);

        auto corruptResult = result;
        const auto corruptBytes = CloudBlobOwnedBytes::Copy(std::array{std::byte{1}, std::byte{2}, std::byte{3}});
        REQUIRE(corruptBytes.HasValue());
        corruptResult.bytes = corruptBytes.Value();
        RequireError(ValidateCloudBlobReadCompletion(corruptResult, request, subject), CloudObjectErrors::IntegrityMismatch);
    }

    TEST_CASE("Cloud read requests use the common request lifecycle for cancellation", "[platform-services][cloud][cancellation]") {
        PlatformRequestStore store{{.activeCapacity = 1, .terminalCapacity = 1, .observerCapacity = 1, .generation = {9}}};
        auto admitted = store.Admit<CloudBlobReadResult>();
        REQUIRE(admitted.HasValue());
        REQUIRE(store.RequestCancel(admitted.Value()).HasValue());
        const auto snapshot = store.Query(admitted.Value());
        REQUIRE(snapshot.HasValue());
        CHECK(snapshot.Value().state == PlatformRequestState::Cancelling);
        CHECK(snapshot.Value().cancellationRequested);
    }
}  // namespace Horo::PlatformServices
