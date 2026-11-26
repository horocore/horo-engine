#include "Horo/Application/CloudSaveTransport.h"
#include "Horo/PlatformServices/PlatformOfflineQueue.h"
#include "Horo/Runtime/Save/SaveCloudRevisionMetadata.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "PlatformCloudTestSupport.h"
#include "PlatformServicesFrontendTestSupport.h"
#include "SaveTestUtils.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using namespace Test;
        namespace Cloud = Horo::PlatformServices;

        static_assert(!std::is_same_v<SaveCloudMutationId, Cloud::CloudMutationId>);
        static_assert(!std::is_constructible_v<Cloud::PlatformOfflineOperation, Cloud::CloudBlobWriteRequest>);
        static_assert(!std::is_constructible_v<Cloud::PlatformOfflineOperation, Cloud::CloudBlobDeleteRequest>);

        [[nodiscard]] SaveCloudMetadataScope Scope(const std::uint8_t profile = 4, const std::uint8_t provider = 5,
                                                   const std::uint8_t account = 6) {
            return {.localNamespace = {.product = Id<ProductStorageId>(1),
                                       .environment = Id<EnvironmentStorageId>(2),
                                       .owner = UserProfileOwner{Id<LocalUserStorageId>(3), Id<GameProfileId>(profile)}},
                    .provider = Id<SaveCloudProviderId>(provider),
                    .account = Id<SaveCloudAccountId>(account)};
        }

        [[nodiscard]] SaveSlotIndex Index(const std::uint64_t revision = 1) {
            return {.revision = revision, .entries = {Entry(1, 10), Entry(2, 20)}};
        }

        [[nodiscard]] SaveCloudRevisionMetadata Metadata(const SaveSlotIndex &index, const SaveCloudMetadataScope &scope = Scope(),
                                                         const std::uint64_t revision = 1) {
            return ReconcileSaveCloudRevisionMetadata(index, scope, std::nullopt, revision).Value();
        }

        [[nodiscard]] SaveCloudObjectRef Object() {
            return {.key = {std::byte{0x00}, std::byte{0xff}}, .revision = std::vector<std::byte>{std::byte{0x7f}}};
        }

        /** @brief Composes the real frontend with finite in-memory cloud mutation capabilities. */
        [[nodiscard]] Cloud::PlatformServicesFrontend CloudFrontend(const std::shared_ptr<Cloud::RoutingBackend> &backend,
                                                                     const Cloud::PlatformSessionSnapshot &session) {
            backend->snapshot = Cloud::Capabilities(session.ProviderGeneration());
            backend->snapshot.services[static_cast<std::size_t>(Cloud::PlatformServiceKind::Cloud)].cloudMutation = Cloud::MutationFacts();
            auto frontend = Cloud::PlatformServicesFrontend::Create(backend, backend->snapshot, session);
            REQUIRE(frontend.HasValue());
            return std::move(frontend).Value();
        }

        TEST_CASE("Save transport dispatches only typed opaque intents from the bound profile",
                  "[unit][save][cloud-transport][frontend]") {
            auto backend = std::make_shared<Cloud::RoutingBackend>();
            const auto session = Cloud::Session();
            auto frontend = CloudFrontend(backend, session);
            const auto transport = Application::CloudSaveTransport::Create(frontend, Scope(), *session.Subject());
            REQUIRE(transport.HasValue());

            const auto slot = Id<SaveGameSlotId>(1);
            const auto mutation = Id<SaveCloudMutationId>(9);
            const auto intent = Cloud::TestSupport::CloudWrite();
            const auto commit = Cloud::TestSupport::CloudCommit(intent);
            REQUIRE(commit.committedObject);
            const auto revision = commit.committedObject->revision;
            const auto listed = transport.Value().List(20, 2);
            const auto read = transport.Value().Read(21, slot, 3);
            const auto write = transport.Value().Write(22, slot, intent.bytes, intent.expectedDigest, mutation, revision);
            const auto deleted = transport.Value().Delete(23, slot, revision, Id<SaveCloudMutationId>(10));
            REQUIRE(listed.HasValue());
            REQUIRE(read.HasValue());
            REQUIRE(write.HasValue());
            REQUIRE(deleted.HasValue());
            CHECK(listed.Value().operation == 20);
            CHECK(read.Value().operation == 21);
            CHECK(write.Value().operation == 22);
            CHECK(deleted.Value().operation == 23);
            REQUIRE(backend->lastCloudList);
            REQUIRE(backend->lastCloudList->prefix);
            REQUIRE(backend->lastCloudRead);
            REQUIRE(backend->lastCloudWrite);
            REQUIRE(backend->lastCloudDelete);

            const auto prefix = backend->lastCloudList->prefix->Bytes();
            const auto key = backend->lastCloudRead->key.Bytes();
            REQUIRE(prefix.size() == 32);
            REQUIRE(key.size() == 64);
            CHECK(std::ranges::equal(key.first(prefix.size()), prefix));
            const auto prefixDigest = ParseSha256("sha256:77eb9bb00a07521bae4c1d7c3efadc80e6682870b6e6ea13e337f780726e8d01");
            const auto slotDigest = ParseSha256("sha256:c4c6e4fcf45224cd48dd885c71bc6a79d852adf2fd367cc585437ddf1e25ee08");
            REQUIRE(prefixDigest.HasValue());
            REQUIRE(slotDigest.HasValue());
            CHECK(std::ranges::equal(prefix, std::as_bytes(std::span{prefixDigest.Value().bytes})));
            CHECK(std::ranges::equal(key.last(prefix.size()), std::as_bytes(std::span{slotDigest.Value().bytes})));
            CHECK(backend->lastCloudRead->maximumBytes == 3);
            CHECK(backend->lastCloudWrite->key == backend->lastCloudRead->key);
            CHECK(backend->lastCloudDelete->key == backend->lastCloudRead->key);
            CHECK(backend->lastCloudWrite->expectedDigest == intent.expectedDigest);
            CHECK(std::ranges::equal(backend->lastCloudWrite->bytes.Bytes(), intent.bytes.Bytes()));
            CHECK(std::get<Cloud::CloudMatchProviderRevision>(backend->lastCloudWrite->precondition).revision == revision);
            CHECK(backend->lastCloudDelete->expectedRevision == revision);
            CHECK(std::ranges::equal(backend->lastCloudWrite->mutation.bytes, std::as_bytes(std::span{mutation.Bytes()})));
            CHECK(backend->lastCloudDelete->mutation.bytes.back() == std::byte{10});
            CHECK(frontend.RequestCancel(write.Value().handle).HasValue());
            CHECK(backend->requests.Query(write.Value().handle).Value().cancellationRequested);

            OperationId reported{};
            auto subscription = backend->requests.OnComplete<Cloud::CloudBlobReadResult>(
                read.Value().handle, [&](const Cloud::PlatformRequestSnapshot<Cloud::CloudBlobReadResult> &snapshot) {
                    if (snapshot.terminal && snapshot.terminal->HasValue())
                        reported = read.Value().operation;
                });
            REQUIRE(subscription.HasValue());
            auto head = *commit.committedObject;
            head.key = backend->lastCloudRead->key;
            const auto subject = *session.Subject();
            REQUIRE(backend->requests.MarkRunning(read.Value().handle).HasValue());
            REQUIRE(backend->requests
                        .CompleteSuccess(read.Value().handle.Id(), read.Value().handle.Generation(),
                                         Cloud::CloudBlobReadResult{subject, subject.SessionGeneration(), std::move(head), intent.bytes})
                        .HasValue());
            CHECK(reported == 0);
            CHECK(backend->requests.DispatchCompletions() == 1);
            CHECK(reported == 21);

            const auto create = transport.Value().Write(24, slot, intent.bytes, intent.expectedDigest, Id<SaveCloudMutationId>(11),
                                                        std::nullopt);
            REQUIRE(create.HasValue());
            CHECK(std::holds_alternative<Cloud::CloudCreateIfAbsent>(backend->lastCloudWrite->precondition));
        }

        TEST_CASE("Save transport key is portable across devices and fenced by typed partition authority",
                  "[unit][save][cloud-transport][frontend]") {
            auto backend = std::make_shared<Cloud::RoutingBackend>();
            const auto session = Cloud::Session();
            auto frontend = Cloud::Frontend(backend, session);
            const auto slot = Id<SaveGameSlotId>(1);
            const auto readKey = [&](const SaveCloudMetadataScope &scope, const SaveGameSlotId requestedSlot) {
                const auto transport = Application::CloudSaveTransport::Create(frontend, scope, *session.Subject());
                REQUIRE(transport.HasValue());
                REQUIRE(transport.Value().Read(1, requestedSlot, 1).HasValue());
                REQUIRE(backend->lastCloudRead);
                return backend->lastCloudRead->key;
            };
            const auto original = readKey(Scope(), slot);
            auto otherDevice = Scope();
            std::get<UserProfileOwner>(otherDevice.localNamespace.owner).user = Id<LocalUserStorageId>(9);
            CHECK(readKey(otherDevice, slot) == original);
            auto otherEnvironment = Scope();
            otherEnvironment.localNamespace.environment = Id<EnvironmentStorageId>(8);
            auto otherProduct = Scope();
            otherProduct.localNamespace.product = Id<ProductStorageId>(8);
            for (const auto &other : {Scope(8), Scope(4, 8), Scope(4, 5, 8), otherEnvironment, otherProduct})
                CHECK(readKey(other, slot) != original);
            CHECK(readKey(Scope(), Id<SaveGameSlotId>(2)) != original);
        }

        TEST_CASE("Save transport rejects unowned, stale, or oversized requests before handing them to the backend",
                  "[unit][save][cloud-transport][frontend]") {
            auto backend = std::make_shared<Cloud::RoutingBackend>();
            const auto session = Cloud::Session();
            auto frontend = CloudFrontend(backend, session);
            const auto transport = Application::CloudSaveTransport::Create(frontend, Scope(), *session.Subject());
            REQUIRE(transport.HasValue());
            const auto intent = Cloud::TestSupport::CloudWrite();
            const auto slot = Id<SaveGameSlotId>(1);
            const auto oldSession = Cloud::Session({7}, {4});
            const auto stale = Application::CloudSaveTransport::Create(frontend, Scope(), *oldSession.Subject());
            REQUIRE(stale.HasValue());
            CHECK(stale.Value().Read(1, slot, 1).HasError());
            CHECK(transport.Value().List(0, 1).HasError());
            CHECK(transport.Value().List(1, 5).HasError());
            CHECK(transport.Value().Read(1, {}, 1).HasError());
            CHECK(transport.Value().Read(1, slot, 5).HasError());
            CHECK(transport.Value().Write(1, slot, intent.bytes, intent.expectedDigest, {}, {}).HasError());
            CHECK(Application::CloudSaveTransport::Create(frontend, {}, *session.Subject()).HasError());
            CHECK(Application::CloudSaveTransport::Create(frontend, Scope(), {}).HasError());
            CHECK(backend->TotalCalls() == 0);
        }

        TEST_CASE("Save transport reports a null cloud capability without touching local state",
                  "[unit][save][cloud-transport][frontend]") {
            auto backend = std::make_shared<Cloud::NullPlatformServicesBackend>(Cloud::PlatformProviderGeneration{7});
            auto capabilities = Cloud::ActivatePlatformServicesBackend(*backend, {});
            REQUIRE(capabilities.HasValue());
            const auto session = Cloud::Session();
            auto created = Cloud::PlatformServicesFrontend::Create(backend, std::move(capabilities).Value(), session);
            REQUIRE(created.HasValue());
            auto frontend = std::move(created).Value();
            const auto transport = Application::CloudSaveTransport::Create(frontend, Scope(), *session.Subject());
            REQUIRE(transport.HasValue());
            Cloud::RequireError(transport.Value().List(1, 1), Cloud::FrontendErrors::NullProvider);
            Cloud::RequireError(transport.Value().Read(1, Id<SaveGameSlotId>(1), 1), Cloud::FrontendErrors::NullProvider);
        }

        /** @brief Copies only a trusted caller-owned record's opaque transport fields and original mutation identity. */
        [[nodiscard]] Cloud::CloudBlobWriteRequest CoordinatorTransportIntent(const SaveCloudGenerationRecord &record,
                                                                              const SaveCloudMutationId &mutation) {
            REQUIRE(record.object.has_value());
            REQUIRE(record.object->revision.has_value());
            auto intent = Cloud::TestSupport::CloudWrite();
            const auto key = Cloud::CloudSaveObjectKey::Copy(record.object->key);
            const auto revision = Cloud::ProviderObjectRevision::Copy(*record.object->revision);
            REQUIRE(key.HasValue());
            REQUIRE(revision.HasValue());
            intent.key = key.Value();
            intent.precondition = Cloud::CloudMatchProviderRevision{revision.Value()};
            std::ranges::transform(mutation.Bytes(), intent.mutation.bytes.begin(), [](const std::uint8_t byte) {
                return static_cast<std::byte>(byte);
            });
            return intent;
        }

        /** @brief Verifies that late commit evidence cannot replace an already published terminal outcome. */
        void RequireImmutableCloudTerminal(Cloud::PlatformRequestStore &requests,
                                           const Cloud::PlatformRequestHandle<Cloud::CloudMutationResult> &handle,
                                           const Cloud::CloudMutationResult &completion, const bool succeeded) {
            const auto terminal = requests.Query(handle);
            REQUIRE(terminal.HasValue());
            REQUIRE(terminal.Value().terminal.has_value());
            CHECK(terminal.Value().terminal->HasValue() == succeeded);
            CHECK(requests.CompleteSuccess(handle.Id(), handle.Generation(), completion).Value() ==
                  Cloud::PlatformRequestMutation::Unchanged);
            CHECK(requests.Query(handle).Value().state == terminal.Value().state);
        }

        TEST_CASE("Cloud transport outcomes do not publish or rewrite Save coordinator state",
                  "[unit][save][cloud-transport][compatibility]") {
            const auto index = Index();
            auto metadata = Metadata(index);
            metadata.records.front().state = SaveCloudGenerationState::Uploading;
            metadata.records.front().object = Object();
            metadata.records.front().lastConfirmed = Id<SaveCloudMutationId>(8);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasValue());
            const auto before = metadata.records;
            auto intent = CoordinatorTransportIntent(metadata.records.front(), Id<SaveCloudMutationId>(9));
            const auto callerOwnedIntent = intent;
            const auto completion = Cloud::TestSupport::CloudCommit(intent);
            REQUIRE(Cloud::ValidateCloudWriteCompletion(completion, intent, intent.subject, {}).HasValue());

            Cloud::PlatformRequestStore requests{{.activeCapacity = 1, .terminalCapacity = 1, .observerCapacity = 1, .generation = {1}}};
            auto admitted = requests.Admit<Cloud::CloudMutationResult>();
            REQUIRE(admitted.HasValue());
            auto handle = std::move(admitted).Value();
            REQUIRE(requests.MarkRunning(handle).HasValue());
            bool succeeded = false;
            SECTION("remote success still requires coordinator-owned local publication") {
                REQUIRE(requests.CompleteSuccess(handle.Id(), handle.Generation(), completion).Value() ==
                        Cloud::PlatformRequestMutation::Applied);
                succeeded = true;
            }
            SECTION("acknowledged cancellation keeps the caller intent") {
                REQUIRE(requests.RequestCancel(handle).HasValue());
                REQUIRE(requests
                            .CompleteCancelled<Cloud::CloudMutationResult>(handle.Id(), handle.Generation(),
                                                                           MakeError(Cloud::RequestErrors::Cancelled))
                            .Value() == Cloud::PlatformRequestMutation::Applied);
            }
            SECTION("timeout preserves uncertainty when commit evidence arrives late") {
                REQUIRE(requests
                            .CompleteTimedOut<Cloud::CloudMutationResult>(handle.Id(), handle.Generation(),
                                                                          MakeError(Cloud::RequestErrors::TimedOut))
                            .Value() == Cloud::PlatformRequestMutation::Applied);
            }
            SECTION("quota does not authorize local cleanup or a new mutation identity") {
                REQUIRE(requests
                            .CompleteFailure<Cloud::CloudMutationResult>(handle.Id(), handle.Generation(),
                                                                         MakeError(Cloud::CloudObjectErrors::QuotaExceeded))
                            .Value() == Cloud::PlatformRequestMutation::Applied);
            }
            RequireImmutableCloudTerminal(requests, handle, completion, succeeded);
            CHECK(Cloud::SameCloudWriteIntent(intent, callerOwnedIntent));
            CHECK(metadata.records == before);
            CHECK(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasValue());
        }

        TEST_CASE("Save cloud evidence is rejected rather than truncated at narrower transport bounds",
                  "[unit][save][cloud-transport][compatibility]") {
            const auto index = Index();
            auto metadata = Metadata(index);
            metadata.records.front().object = Object();
            auto &object = *metadata.records.front().object;
            SECTION("opaque key") {
                object.key.assign(Cloud::MaximumCloudObjectAddressBytes + 1, std::byte{0xff});
                REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasValue());
                const auto original = object;
                const auto converted = Cloud::CloudSaveObjectKey::Copy(object.key);
                REQUIRE(converted.HasError());
                CHECK(converted.ErrorValue().code.Value() == Cloud::CloudObjectErrors::InvalidOpaqueValue.code.Value());
                CHECK(object == original);
            }
            SECTION("opaque revision") {
                object.revision->assign(Cloud::MaximumCloudObjectAddressBytes + 1, std::byte{0xfe});
                REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasValue());
                const auto original = object;
                const auto converted = Cloud::ProviderObjectRevision::Copy(*object.revision);
                REQUIRE(converted.HasError());
                CHECK(converted.ErrorValue().code.Value() == Cloud::CloudObjectErrors::InvalidOpaqueValue.code.Value());
                CHECK(object == original);
            }
        }

        TEST_CASE("A cloud sidecar is bound to the exact index revision generation and archive", "[unit][save][cloud-metadata]") {
            auto index = Index();
            auto metadata = Metadata(index);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasValue());

            metadata.indexRevision = 2;
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataStale.code.Value());
            metadata.indexRevision = 1;
            metadata.records[0].generation = Id<SlotGenerationId>(11);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataStale.code.Value());
            metadata.records[0].generation = Id<SlotGenerationId>(10);
            metadata.records[0].archive = Entry(1, 11).publication.archiveContent;
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataStale.code.Value());
        }

        TEST_CASE("Profiles providers and accounts cannot borrow one another's cloud evidence", "[unit][save][cloud-metadata]") {
            const auto index = Index();
            auto prior = Metadata(index);
            prior.records[0].state = SaveCloudGenerationState::Clean;
            prior.records[0].object = Object();
            prior.records[0].lastConfirmed = Id<SaveCloudMutationId>(8);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(7), prior).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataInvalid.code.Value());
            REQUIRE(SaveCloudRevisionSnapshot::Create(index, Scope(7), prior).HasError());

            for (const auto scope : {Scope(7, 5, 6), Scope(4, 7, 6), Scope(4, 5, 7)}) {
                const auto rebuilt = ReconcileSaveCloudRevisionMetadata(index, scope, prior, 2).Value();
                REQUIRE(rebuilt.records[0].state == SaveCloudGenerationState::Unknown);
                REQUIRE_FALSE(rebuilt.records[0].object);
                REQUIRE_FALSE(rebuilt.records[0].lastConfirmed);
            }
            const auto retained = ReconcileSaveCloudRevisionMetadata(index, Scope(), prior, 2).Value();
            REQUIRE(retained.records[0].state == SaveCloudGenerationState::Clean);
            REQUIRE(retained.records[0].object == prior.records[0].object);
            REQUIRE(retained.records[0].lastConfirmed == prior.records[0].lastConfirmed);
        }

        TEST_CASE("Reconciliation resets replaced generations and interrupted transfers", "[unit][save][cloud-metadata]") {
            auto oldIndex = Index();
            auto prior = Metadata(oldIndex);
            prior.records[0].state = SaveCloudGenerationState::Uploading;
            prior.records[0].object = Object();
            prior.records[1].state = SaveCloudGenerationState::Dirty;
            prior.records[1].object = Object();

            const auto restarted = ReconcileSaveCloudRevisionMetadata(oldIndex, Scope(), prior, 2).Value();
            REQUIRE(restarted.records[0].state == SaveCloudGenerationState::Unknown);
            REQUIRE(restarted.records[0].object == prior.records[0].object);
            REQUIRE(restarted.records[1].state == SaveCloudGenerationState::Dirty);

            auto current = Index(2);
            current.entries[1] = Entry(2, 21);
            const auto rebuilt = ReconcileSaveCloudRevisionMetadata(current, Scope(), prior, 2).Value();
            REQUIRE(rebuilt.indexRevision == 2);
            REQUIRE(rebuilt.records[0].state == SaveCloudGenerationState::Unknown);
            REQUIRE(rebuilt.records[0].object == prior.records[0].object);
            REQUIRE(rebuilt.records[1].generation == Id<SlotGenerationId>(21));
            REQUIRE(rebuilt.records[1].state == SaveCloudGenerationState::Unknown);
            REQUIRE_FALSE(rebuilt.records[1].object);
            REQUIRE(ValidateSaveCloudRevisionMetadata(current, Scope(), rebuilt).HasValue());
        }

        TEST_CASE("A missing or invalid sidecar rebuilds without inventing remote authority", "[unit][save][cloud-metadata]") {
            const auto index = Index(3);
            const auto absent = ReconcileSaveCloudRevisionMetadata(index, Scope(), std::nullopt, 1).Value();
            REQUIRE(absent.records.size() == 2);
            REQUIRE_FALSE(absent.records[0].object);
            REQUIRE(absent.records[0].state == SaveCloudGenerationState::Unknown);

            auto corrupt = absent;
            corrupt.schemaVersion = 99;
            corrupt.records[0].state = SaveCloudGenerationState::Clean;
            corrupt.records[0].object = Object();
            const auto rebuilt = ReconcileSaveCloudRevisionMetadata(index, Scope(), corrupt, 2).Value();
            REQUIRE(rebuilt.records[0].state == SaveCloudGenerationState::Unknown);
            REQUIRE_FALSE(rebuilt.records[0].object);
        }

        TEST_CASE("An unchanged generation retains confirmed provider evidence across an index revision", "[unit][save][cloud-metadata]") {
            const auto oldIndex = Index();
            auto prior = Metadata(oldIndex);
            prior.records[0].state = SaveCloudGenerationState::Clean;
            prior.records[0].object = Object();
            const auto current = Index(2);
            const auto rebuilt = ReconcileSaveCloudRevisionMetadata(current, Scope(), prior, 2).Value();
            REQUIRE(rebuilt.records[0].state == SaveCloudGenerationState::Clean);
            REQUIRE(rebuilt.records[0].object == prior.records[0].object);
            REQUIRE(ValidateSaveCloudRevisionMetadata(current, Scope(), rebuilt).HasValue());
        }

        TEST_CASE("Reconciliation never rolls a newer confirmed sidecar back onto an older index", "[unit][save][cloud-metadata]") {
            const auto current = Index(2);
            auto confirmed = Metadata(current);
            confirmed.records[0].state = SaveCloudGenerationState::Clean;
            confirmed.records[0].object = Object();
            const auto older = Index(1);
            REQUIRE(ReconcileSaveCloudRevisionMetadata(older, Scope(), confirmed, 2).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataStale.code.Value());
            REQUIRE(confirmed.records[0].state == SaveCloudGenerationState::Clean);
        }

        TEST_CASE("Validation rejects malformed and unbounded provider evidence", "[unit][save][cloud-metadata]") {
            const auto index = Index();
            auto metadata = Metadata(index);
            metadata.records[0].state = SaveCloudGenerationState::Clean;
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataInvalid.code.Value());
            metadata.records[0].object = Object();
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasValue());
            metadata.records[0].object->key.clear();
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasError());
            metadata.records[0].object->key.resize(513);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataLimitExceeded.code.Value());
            metadata.records[0].object = Object();
            metadata.records[0].object->revision->resize(513);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataLimitExceeded.code.Value());
            metadata.records[0].object = Object();
            metadata.records[0].state = static_cast<SaveCloudGenerationState>(255);
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasError());
        }

        TEST_CASE("Every declared sync state remains generation-bound", "[unit][save][cloud-metadata]") {
            const auto index = Index();
            auto metadata = Metadata(index);
            for (const auto state :
                 {SaveCloudGenerationState::Unknown, SaveCloudGenerationState::Clean, SaveCloudGenerationState::Dirty,
                  SaveCloudGenerationState::Uploading, SaveCloudGenerationState::Downloading, SaveCloudGenerationState::Conflicted,
                  SaveCloudGenerationState::Deleted, SaveCloudGenerationState::Failed}) {
                metadata.records[0].state = state;
                metadata.records[0].object = Object();
                REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasValue());
                REQUIRE(metadata.records[0].generation == index.entries[0].publication.generation);
            }
        }

        TEST_CASE("Snapshot owns a deep immutable copy of the validated index and sidecar", "[unit][save][cloud-metadata]") {
            auto index = Index();
            auto metadata = Metadata(index);
            metadata.records[0].object = Object();
            const auto snapshot = SaveCloudRevisionSnapshot::Create(index, Scope(), metadata).Value();
            index.entries[0] = Entry(1, 11);
            metadata.records[0].object->key[0] = std::byte{0x55};
            REQUIRE(snapshot.Index().entries[0].publication.generation == Id<SlotGenerationId>(10));
            REQUIRE(snapshot.Metadata().records[0].object->key[0] == std::byte{0x00});
            REQUIRE(ValidateSaveCloudRevisionMetadata(snapshot.Index(), Scope(), snapshot.Metadata()).HasValue());
        }

        TEST_CASE("Invalid bounds and duplicate records fail closed", "[unit][save][cloud-metadata]") {
            const auto index = Index();
            auto metadata = Metadata(index);
            metadata.records[1] = metadata.records[0];
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), metadata).HasError());
            const SaveCloudRevisionMetadataLimits limits{.maximumRecords = 1};
            REQUIRE(ValidateSaveCloudRevisionMetadata(index, Scope(), Metadata(index), limits).ErrorValue().code.Value() ==
                    SaveErrors::CloudMetadataLimitExceeded.code.Value());
            REQUIRE(ReconcileSaveCloudRevisionMetadata(index, Scope(), std::nullopt, 1, limits).HasError());
            REQUIRE(SaveCloudRevisionSnapshot::Create(index, Scope(), metadata).HasError());
        }
    }  // namespace
}  // namespace Horo::Runtime
