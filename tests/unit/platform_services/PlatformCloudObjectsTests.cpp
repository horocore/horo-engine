#include "Horo/PlatformServices/PlatformCloudObjects.h"
#include "PlatformServicesTestSupport.h"

#include <array>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <map>
#include <mutex>
#include <thread>
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

        CloudMutationCapability MutationCapability() {
            return {.atomicity = CloudMutationAtomicity::ConditionalAtomicObject,
                    .createIfAbsent = true,
                    .replaceIfRevision = true,
                    .deleteIfRevision = true,
                    .durableMutationDedupe = true,
                    .maxNamespaceBytes = 32,
                    .maxObjectCount = 4,
                    .maxConcurrentMutations = 2};
        }

        CloudMutationId Mutation() {
            CloudMutationId id;
            id.bytes.back() = std::byte{9};
            return id;
        }

        // A deterministic provider model: the lock represents one remote atomic commit point.
        // Product adapters must qualify equivalent behavior using their native conditional primitive.
        class AtomicCloudProviderModel final {
        public:
            Result<CloudMutationResult> Write(const CloudBlobWriteRequest &request) {
                std::lock_guard lock(mutex_);
                if (const auto replay = Replayed(request.mutation, CloudMutationIntent{request}); replay)
                    return *replay;
                if (std::holds_alternative<CloudCreateIfAbsent>(request.precondition) && head_)
                    return Remember(request.mutation, request,
                                    Result<CloudMutationResult>::Failure(MakeError(CloudObjectErrors::AlreadyExists)));
                if (const auto *matched = std::get_if<CloudMatchProviderRevision>(&request.precondition);
                    matched && (!head_ || head_->revision != matched->revision))
                    return Remember(request.mutation, request,
                                    Result<CloudMutationResult>::Failure(MakeError(CloudObjectErrors::PreconditionFailed)));
                if (request.bytes.Bytes().size() > 32 || otherUsage_ > 32 - request.bytes.Bytes().size())
                    return Remember(request.mutation, request,
                                    Result<CloudMutationResult>::Failure(MakeError(CloudObjectErrors::QuotaExceeded)));
                const auto revision = Revision(static_cast<std::byte>(++revision_));
                head_ = {.key = request.key,
                         .revision = revision,
                         .sizeBytes = request.bytes.Bytes().size(),
                         .transportDigest = request.expectedDigest};
                ++commits_;
                return Remember(request.mutation, request,
                                Result<CloudMutationResult>::Success({.subject = request.subject,
                                                                      .sessionGeneration = request.subject.SessionGeneration(),
                                                                      .key = request.key,
                                                                      .mutation = request.mutation,
                                                                      .committedObject = head_}));
            }

            Result<CloudMutationResult> Delete(const CloudBlobDeleteRequest &request) {
                std::lock_guard lock(mutex_);
                if (const auto replay = Replayed(request.mutation, CloudMutationIntent{request}); replay)
                    return *replay;
                if (!head_ || head_->revision != request.expectedRevision)
                    return Remember(request.mutation, request,
                                    Result<CloudMutationResult>::Failure(MakeError(CloudObjectErrors::PreconditionFailed)));
                head_.reset();
                ++commits_;
                return Remember(request.mutation, request,
                                Result<CloudMutationResult>::Success({.subject = request.subject,
                                                                      .sessionGeneration = request.subject.SessionGeneration(),
                                                                      .key = request.key,
                                                                      .mutation = request.mutation}));
            }

            [[nodiscard]] std::uint32_t Commits() const {
                std::lock_guard lock(mutex_);
                return commits_;
            }

            void SetOtherUsage(const std::size_t usedBytes) {
                std::lock_guard lock(mutex_);
                otherUsage_ = usedBytes;
            }

        private:
            using Outcome = Result<CloudMutationResult>;

            struct LedgerEntry final {
                CloudMutationIntent intent;
                Outcome outcome;
            };

            [[nodiscard]] std::optional<Outcome> Replayed(const CloudMutationId &id, const CloudMutationIntent &intent) const {
                const auto found = ledger_.find(id);
                if (found == ledger_.end())
                    return std::nullopt;
                if (ValidateCloudMutationReplay(found->second.intent, intent).HasError())
                    return Outcome::Failure(MakeError(CloudObjectErrors::IdempotencyConflict));
                return found->second.outcome;
            }

            [[nodiscard]] Outcome Remember(const CloudMutationId &id, CloudMutationIntent intent, Outcome outcome) {
                ledger_.emplace(id, LedgerEntry{std::move(intent), outcome});
                return outcome;
            }

            mutable std::mutex mutex_;
            std::map<CloudMutationId, LedgerEntry> ledger_;
            std::optional<CloudObjectHead> head_;
            std::uint8_t revision_{};
            std::uint32_t commits_{};
            std::size_t otherUsage_{};
        };
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

    TEST_CASE("Cloud mutation admission requires atomic CAS dedupe and exact immutable bytes", "[platform-services][cloud][mutation]") {
        const auto subject = Subject(4, std::byte{1});
        const std::array bytes{std::byte{1}, std::byte{2}};
        const auto owned = CloudBlobOwnedBytes::Copy(bytes);
        REQUIRE(owned.HasValue());
        CloudBlobWriteRequest write{.subject = subject,
                                    .key = Key(std::byte{3}),
                                    .bytes = owned.Value(),
                                    .expectedDigest = ComputeSha256(bytes),
                                    .precondition = CloudCreateIfAbsent{},
                                    .mutation = Mutation()};
        auto capability = MutationCapability();
        REQUIRE(ValidateCloudBlobWriteRequest(write, capability, {}).HasValue());
        auto missingPrecondition = write;
        missingPrecondition.precondition = {};
        RequireError(ValidateCloudBlobWriteRequest(missingPrecondition, capability, {}), CloudObjectErrors::InvalidRequest);
        CHECK(SameCloudWriteIntent(write, write));
        auto changedIntent = write;
        changedIntent.key = Key(std::byte{7});
        CHECK_FALSE(SameCloudWriteIntent(write, changedIntent));
        changedIntent = write;
        changedIntent.precondition = CloudMatchProviderRevision{Revision(std::byte{4})};
        CHECK_FALSE(SameCloudWriteIntent(write, changedIntent));
        write.precondition = CloudMatchProviderRevision{Revision(std::byte{4})};
        REQUIRE(ValidateCloudBlobWriteRequest(write, capability, {}).HasValue());
        write.expectedDigest = ComputeSha256(std::array{std::byte{7}});
        RequireError(ValidateCloudBlobWriteRequest(write, capability, {}), CloudObjectErrors::IntegrityMismatch);
        write.expectedDigest = ComputeSha256(bytes);
        const auto completeBytes = write.bytes;
        write.bytes = {};
        RequireError(ValidateCloudBlobWriteRequest(write, capability, {}), CloudObjectErrors::InvalidRequest);
        write.bytes = completeBytes;
        write.mutation = {};
        RequireError(ValidateCloudBlobWriteRequest(write, capability, {}), CloudObjectErrors::InvalidRequest);
        write.mutation = Mutation();
        capability.atomicity = CloudMutationAtomicity::UncoordinatedBlob;
        RequireError(ValidateCloudBlobWriteRequest(write, capability, {}), CloudObjectErrors::InvalidLimits);
        capability.createIfAbsent = false;
        capability.replaceIfRevision = false;
        capability.deleteIfRevision = false;
        capability.durableMutationDedupe = false;
        RequireError(ValidateCloudBlobWriteRequest(write, capability, {}), CloudObjectErrors::UnsupportedCapability);
        capability = MutationCapability();
        capability.durableMutationDedupe = false;
        RequireError(ValidateCloudBlobWriteRequest(write, capability, {}), CloudObjectErrors::InvalidLimits);
        capability = MutationCapability();
        capability.maxNamespaceBytes = 1;
        RequireError(ValidateCloudBlobWriteRequest(write, capability, {}), CloudObjectErrors::PayloadTooLarge);
        capability = MutationCapability();
        capability.maxObjectCount = 0;
        RequireError(ValidateCloudMutationCapability(capability, {}), CloudObjectErrors::InvalidLimits);
        capability = MutationCapability();
        CHECK(ValidateCloudQuotaObservation({.subject = subject,
                                             .sessionGeneration = subject.SessionGeneration(),
                                             .usedBytes = 31,
                                             .objectCount = 3},
                                            capability, subject)
                  .HasValue());
        CHECK(ValidateCloudQuotaObservation({.subject = subject, .sessionGeneration = subject.SessionGeneration()}, capability, subject)
                  .HasValue());
        RequireError(ValidateCloudQuotaObservation({.subject = subject, .sessionGeneration = subject.SessionGeneration(), .usedBytes = 33},
                                                   capability, subject),
                     CloudObjectErrors::InvalidLimits);
        RequireError(ValidateCloudQuotaObservation({.subject = subject, .sessionGeneration = {5}}, capability, subject),
                     CloudObjectErrors::StaleSession);
    }

    TEST_CASE("Cloud mutation completion requires current session and exact atomic evidence", "[platform-services][cloud][mutation]") {
        const auto subject = Subject(4, std::byte{1});
        const std::array bytes{std::byte{1}, std::byte{2}};
        const auto owned = CloudBlobOwnedBytes::Copy(bytes);
        REQUIRE(owned.HasValue());
        const auto digest = ComputeSha256(bytes);
        CloudBlobWriteRequest write{.subject = subject,
                                    .key = Key(std::byte{3}),
                                    .bytes = owned.Value(),
                                    .expectedDigest = digest,
                                    .precondition = CloudMatchProviderRevision{Revision(std::byte{4})},
                                    .mutation = Mutation()};
        CloudMutationResult result{.subject = subject,
                                   .sessionGeneration = subject.SessionGeneration(),
                                   .key = write.key,
                                   .mutation = write.mutation,
                                   .committedObject = Head(std::byte{3}, std::byte{5}, 2, digest)};
        REQUIRE(ValidateCloudWriteCompletion(result, write, subject, {}).HasValue());
        RequireError(ValidateCloudWriteCompletion(result, write, Subject(5, std::byte{1}), {}), CloudObjectErrors::StaleSession);
        result.committedObject->revision = Revision(std::byte{4});
        RequireError(ValidateCloudWriteCompletion(result, write, subject, {}), CloudObjectErrors::InvalidProviderResponse);
        result.committedObject->revision = Revision(std::byte{5});
        result.committedObject->transportDigest.reset();
        RequireError(ValidateCloudWriteCompletion(result, write, subject, {}), CloudObjectErrors::InvalidProviderResponse);

        CloudBlobDeleteRequest remove{.subject = subject,
                                      .key = write.key,
                                      .expectedRevision = Revision(std::byte{5}),
                                      .mutation = Mutation()};
        result.committedObject.reset();
        REQUIRE(ValidateCloudBlobDeleteRequest(remove, MutationCapability(), {}).HasValue());
        CHECK(SameCloudDeleteIntent(remove, remove));
        auto changedDelete = remove;
        changedDelete.expectedRevision = Revision(std::byte{6});
        CHECK_FALSE(SameCloudDeleteIntent(remove, changedDelete));
        REQUIRE(ValidateCloudDeleteCompletion(result, remove, subject).HasValue());
        result.committedObject = Head(std::byte{3}, std::byte{5}, 2);
        RequireError(ValidateCloudDeleteCompletion(result, remove, subject), CloudObjectErrors::InvalidProviderResponse);
        remove.expectedRevision = {};
        RequireError(ValidateCloudBlobDeleteRequest(remove, MutationCapability(), {}), CloudObjectErrors::InvalidRequest);
    }

    TEST_CASE("Atomic cloud provider contract rejects concurrent stale revisions and deduplicates exact retries",
              "[platform-services][cloud][mutation][concurrency]") {
        AtomicCloudProviderModel provider;
        const auto subject = Subject(4, std::byte{1});
        const auto owned = CloudBlobOwnedBytes::Copy(std::array{std::byte{1}});
        REQUIRE(owned.HasValue());
        CloudBlobWriteRequest create{.subject = subject,
                                     .key = Key(std::byte{3}),
                                     .bytes = owned.Value(),
                                     .expectedDigest = ComputeSha256(owned.Value().Bytes()),
                                     .precondition = CloudCreateIfAbsent{},
                                     .mutation = Mutation()};
        const auto created = provider.Write(create);
        REQUIRE(created.HasValue());
        REQUIRE(created.Value().committedObject.has_value());
        const auto firstRevision = created.Value().committedObject->revision;
        const auto exactReplay = provider.Write(create);
        REQUIRE(exactReplay.HasValue());
        CHECK(exactReplay.Value().committedObject->revision == firstRevision);
        CHECK(provider.Commits() == 1);

        auto changed = create;
        const auto changedBytes = CloudBlobOwnedBytes::Copy(std::array{std::byte{2}});
        REQUIRE(changedBytes.HasValue());
        changed.bytes = changedBytes.Value();
        changed.expectedDigest = ComputeSha256(changed.bytes.Bytes());
        RequireError(provider.Write(changed), CloudObjectErrors::IdempotencyConflict);
        changed = create;
        changed.key = Key(std::byte{7});
        RequireError(provider.Write(changed), CloudObjectErrors::IdempotencyConflict);

        CloudBlobWriteRequest replaceA = create;
        replaceA.precondition = CloudMatchProviderRevision{firstRevision};
        replaceA.mutation.bytes.back() = std::byte{10};
        CloudBlobWriteRequest replaceB = replaceA;
        replaceB.mutation.bytes.back() = std::byte{11};
        std::barrier start(3);
        std::optional<Result<CloudMutationResult>> resultA;
        std::optional<Result<CloudMutationResult>> resultB;
        std::thread first([&] {
            start.arrive_and_wait();
            resultA = provider.Write(replaceA);
        });
        std::thread second([&] {
            start.arrive_and_wait();
            resultB = provider.Write(replaceB);
        });
        start.arrive_and_wait();
        first.join();
        second.join();
        REQUIRE(resultA.has_value());
        REQUIRE(resultB.has_value());
        REQUIRE(resultA->HasValue() != resultB->HasValue());
        if (resultA->HasError())
            RequireError(*resultA, CloudObjectErrors::PreconditionFailed);
        if (resultB->HasError())
            RequireError(*resultB, CloudObjectErrors::PreconditionFailed);
        const auto loser = resultA->HasError() ? provider.Write(replaceA) : provider.Write(replaceB);
        RequireError(loser, CloudObjectErrors::PreconditionFailed);
        CHECK(provider.Commits() == 2);
        const auto winner = resultA->HasValue() ? *resultA : *resultB;
        REQUIRE(winner.Value().committedObject.has_value());
        CHECK(winner.Value().committedObject->revision != firstRevision);

        CloudBlobDeleteRequest staleDelete{.subject = subject,
                                           .key = create.key,
                                           .expectedRevision = firstRevision,
                                           .mutation = Mutation()};
        staleDelete.mutation.bytes.back() = std::byte{13};
        RequireError(provider.Delete(staleDelete), CloudObjectErrors::PreconditionFailed);
        CHECK(provider.Commits() == 2);

        CloudBlobDeleteRequest remove{.subject = subject,
                                      .key = create.key,
                                      .expectedRevision = winner.Value().committedObject->revision,
                                      .mutation = Mutation()};
        remove.mutation.bytes.back() = std::byte{12};
        const auto deleted = provider.Delete(remove);
        REQUIRE(deleted.HasValue());
        CHECK_FALSE(deleted.Value().committedObject.has_value());
        CHECK(provider.Delete(remove).HasValue());
        CHECK(provider.Commits() == 3);
        auto conflictingDelete = remove;
        conflictingDelete.mutation = create.mutation;
        RequireError(provider.Delete(conflictingDelete), CloudObjectErrors::IdempotencyConflict);
        CHECK(provider.Commits() == 3);
    }

    TEST_CASE("Atomic cloud provider contract retains a quota outcome for exact mutation replay", "[platform-services][cloud][quota]") {
        AtomicCloudProviderModel provider;
        const auto subject = Subject(4, std::byte{1});
        const auto bytes = CloudBlobOwnedBytes::Copy(std::array{std::byte{1}});
        REQUIRE(bytes.HasValue());
        CloudBlobWriteRequest write{.subject = subject,
                                    .key = Key(std::byte{3}),
                                    .bytes = bytes.Value(),
                                    .expectedDigest = ComputeSha256(bytes.Value().Bytes()),
                                    .precondition = CloudCreateIfAbsent{},
                                    .mutation = Mutation()};
        provider.SetOtherUsage(32);
        RequireError(provider.Write(write), CloudObjectErrors::QuotaExceeded);
        provider.SetOtherUsage(0);
        RequireError(provider.Write(write), CloudObjectErrors::QuotaExceeded);
        CHECK(provider.Commits() == 0);
    }

    TEST_CASE("Cloud mutations retain cancellation state until provider retirement", "[platform-services][cloud][cancellation]") {
        PlatformRequestStore store{{.activeCapacity = 1, .terminalCapacity = 1, .observerCapacity = 1, .generation = {9}}};
        auto admitted = store.Admit<CloudMutationResult>();
        REQUIRE(admitted.HasValue());
        REQUIRE(store.RequestCancel(admitted.Value()).HasValue());
        const auto snapshot = store.Query(admitted.Value());
        REQUIRE(snapshot.HasValue());
        CHECK(snapshot.Value().state == PlatformRequestState::Cancelling);
        CHECK(snapshot.Value().cancellationRequested);
    }
}  // namespace Horo::PlatformServices
