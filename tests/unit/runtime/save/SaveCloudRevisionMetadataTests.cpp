#include "Horo/Application/CloudSaveMutationJournal.h"
#include "Horo/Application/CloudSaveTransport.h"
#include "Horo/Application/CloudSaveUpload.h"
#include "Horo/Platform/NativeImmutableFileStorage.h"
#include "Horo/PlatformServices/PlatformOfflineQueue.h"
#include "Horo/Runtime/Save/SaveCloudRevisionMetadata.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "OwnedTestDirectory.h"
#include "PlatformCloudTestSupport.h"
#include "PlatformServicesFrontendTestSupport.h"
#include "SaveSlotLifecycleTestSupport.h"
#include "SaveTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
                                                                     const Cloud::PlatformSessionSnapshot &session,
                                                                     const std::uint64_t maximumPayload = 4) {
            backend->snapshot = Cloud::Capabilities(session.ProviderGeneration());
            auto &cloud = backend->snapshot.services[static_cast<std::size_t>(Cloud::PlatformServiceKind::Cloud)];
            cloud.limits.maxPayloadBytes = maximumPayload;
            cloud.cloudMutation = Cloud::MutationFacts();
            cloud.cloudMutation->maxNamespaceBytes = std::max(cloud.cloudMutation->maxNamespaceBytes, maximumPayload);
            auto frontend = Cloud::PlatformServicesFrontend::Create(backend, backend->snapshot, session);
            REQUIRE(frontend.HasValue());
            return std::move(frontend).Value();
        }

        [[nodiscard]] Application::CloudSaveTransport BoundTransport(Cloud::PlatformServicesFrontend &frontend,
                                                                      const Cloud::PlatformSessionSnapshot &session,
                                                                      const SaveCloudMetadataScope &scope = Scope(),
                                                                      const std::uint64_t bindingRevision = 1) {
            auto transport = Application::CloudSaveTransport::Create(
                frontend, scope, *session.Subject(), {scope.localNamespace, bindingRevision});
            REQUIRE(transport.HasValue());
            return std::move(transport).Value();
        }

        [[nodiscard]] Application::CloudSaveMutationJournal NativeJournal(const Tests::OwnedTestDirectory &directory,
                                                                          Application::CloudSaveTransport &transport) {
            auto storage = Platform::CreateNativeImmutableFileStorage(directory.Path(), "save-cloud");
            REQUIRE(storage.HasValue());
            auto journal = Application::CloudSaveMutationJournal::Bind(std::move(storage).Value(), transport);
            REQUIRE(journal.HasValue());
            return std::move(journal).Value();
        }

        [[nodiscard]] Application::CloudSaveMutationIntent UploadIntent(const Cloud::CloudBlobWriteRequest &request) {
            return {.mutation = Id<SaveCloudMutationId>(9),
                    .slot = Id<SaveGameSlotId>(1),
                    .generation = Id<SlotGenerationId>(2),
                    .parent = Id<SlotGenerationId>(1),
                    .archive = ArchiveContentHash{Digest(19)},
                    .upload = Application::CloudSaveUploadEvidence{request.expectedDigest, request.bytes.Bytes().size()}};
        }

        /** @brief Delivers a scripted receipt through the admitted mutation's owning request store. */
        [[nodiscard]] Cloud::PlatformRequestSnapshot<Cloud::CloudMutationResult> ObserveWrite(
            Cloud::RoutingBackend &backend, const Cloud::PlatformRequestHandle<Cloud::CloudMutationResult> &handle,
            const Cloud::CloudMutationResult &completion) {
            REQUIRE(backend.requests.MarkRunning(handle).HasValue());
            REQUIRE(backend.requests.CompleteSuccess(handle.Id(), handle.Generation(), completion).HasValue());
            auto snapshot = backend.requests.Query(handle);
            REQUIRE(snapshot.HasValue());
            return std::move(snapshot).Value();
        }

        TEST_CASE("Cloud head comparison separates publication history from gameplay equivalence", "[unit][save][cloud-conflict]") {
            using enum SaveCloudHeadRelation;
            const auto slot = Id<SaveGameSlotId>(1);
            const auto common = Id<SlotGenerationId>(8);
            const SaveCloudPublicationHead local{slot, Id<SlotGenerationId>(9), common, ArchiveContentHash{Digest(1)},
                                                 CanonicalStateHash{Digest(2)}};
            CHECK(CompareSaveCloudHeads(local, local) == SamePublication);
            CHECK(CompareSaveCloudHeads(local, {}) == Unproven);

            auto remote = local;
            remote.archive = ArchiveContentHash{Digest(3)};
            CHECK(CompareSaveCloudHeads(local, remote) == IntegrityMismatch);
            remote.generation = Id<SlotGenerationId>(10);
            CHECK(CompareSaveCloudHeads(local, remote) == EquivalentState);
            remote.state = CanonicalStateHash{Digest(4)};
            CHECK(CompareSaveCloudHeads(local, remote) == Diverged);
            remote.parent.reset();
            CHECK(CompareSaveCloudHeads(local, remote) == Unproven);
            remote.parent = local.generation;
            CHECK(CompareSaveCloudHeads(local, remote) == RemoteAhead);
            CHECK(CompareSaveCloudHeads(remote, local) == LocalAhead);

            const SaveCloudDeletionHead deleted{slot, local.generation, local.archive};
            CHECK(CompareSaveCloudHeads(local, deleted) == RemoteDeleted);
            CHECK(CompareSaveCloudHeads(deleted, local) == LocalDeleted);
            CHECK(CompareSaveCloudHeads(deleted, remote) == DeleteEdit);
            CHECK(CompareSaveCloudHeads(deleted, deleted) == BothDeleted);
            CHECK(CompareSaveCloudHeads(deleted, {}) == Unproven);
            remote.slot = Id<SaveGameSlotId>(2);
            CHECK(CompareSaveCloudHeads(local, remote) == IntegrityMismatch);
        }

        TEST_CASE("Save transport dispatches only typed opaque intents from the bound profile",
                  "[unit][save][cloud-transport][frontend]") {
            auto backend = std::make_shared<Cloud::RoutingBackend>();
            const auto session = Cloud::Session();
            auto frontend = CloudFrontend(backend, session);
            auto transport = BoundTransport(frontend, session);
            const Tests::OwnedTestDirectory directory{"save-cloud-transport"};
            const auto journal = NativeJournal(directory, transport);

            const auto slot = Id<SaveGameSlotId>(1);
            const auto mutation = Id<SaveCloudMutationId>(9);
            const auto intent = Cloud::TestSupport::CloudWrite();
            const auto commit = Cloud::TestSupport::CloudCommit(intent);
            REQUIRE(commit.committedObject);
            const auto revision = commit.committedObject->revision;
            const auto listed = transport.List(20, 2);
            const auto read = transport.Read(21, slot, 3);
            auto upload = UploadIntent(intent);
            upload.revision = revision;
            const auto durableUpload = journal.Store(upload);
            REQUIRE(durableUpload.HasValue());
            auto deletion = upload;
            deletion.mutation = Id<SaveCloudMutationId>(10);
            deletion.upload.reset();
            const auto durableDelete = journal.Store(deletion);
            REQUIRE(durableDelete.HasValue());
            const auto write = transport.DispatchMutation(22, durableUpload.Value(), intent.bytes);
            const auto deleted = transport.DispatchMutation(23, durableDelete.Value());
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
            auto subscription = transport.OnComplete(
                read.Value(), [&](const OperationId operation, const Cloud::PlatformRequestSnapshot<Cloud::CloudBlobReadResult> &snapshot) {
                    if (snapshot.terminal && snapshot.terminal->HasValue())
                        reported = operation;
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

            std::vector<OperationId> failed;
            const auto onFailure = [&failed](const OperationId operation, const auto &snapshot) {
                REQUIRE(snapshot.terminal);
                REQUIRE(snapshot.terminal->HasError());
                failed.push_back(operation);
            };
            const auto listObserver = transport.OnComplete(listed.Value(), onFailure);
            const auto deleteObserver = transport.OnComplete(deleted.Value(), onFailure);
            REQUIRE(listObserver.HasValue());
            REQUIRE(deleteObserver.HasValue());
            REQUIRE(backend->requests.MarkRunning(listed.Value().handle).HasValue());
            REQUIRE(backend->requests.MarkRunning(deleted.Value().handle).HasValue());
            REQUIRE(backend->requests.CompleteFailure(listed.Value().handle, MakeError(Cloud::CloudObjectErrors::PartialTransfer)).HasValue());
            REQUIRE(backend->requests.CompleteFailure(deleted.Value().handle, MakeError(Cloud::CloudObjectErrors::PreconditionFailed)).HasValue());
            CHECK(failed.empty());
            CHECK(backend->requests.DispatchCompletions() == 2);
            CHECK(failed == std::vector<OperationId>{20, 23});

            upload.mutation = Id<SaveCloudMutationId>(11);
            upload.revision.reset();
            const auto durableCreate = journal.Store(upload);
            REQUIRE(durableCreate.HasValue());
            const auto create = transport.DispatchMutation(24, durableCreate.Value(), intent.bytes);
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
                const auto transport = Application::CloudSaveTransport::Create(frontend, scope, *session.Subject(), {scope.localNamespace, 1});
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
            const SaveNamespaceAccessRequest access{Scope().localNamespace, 1};
            const auto transport = Application::CloudSaveTransport::Create(frontend, Scope(), *session.Subject(), access);
            REQUIRE(transport.HasValue());
            const auto slot = Id<SaveGameSlotId>(1);
            const auto oldSession = Cloud::Session({7}, {4});
            const auto stale = Application::CloudSaveTransport::Create(frontend, Scope(), *oldSession.Subject(), access);
            REQUIRE(stale.HasValue());
            CHECK(stale.Value().Read(1, slot, 1).HasError());
            CHECK(transport.Value().List(0, 1).HasError());
            CHECK(transport.Value().List(1, 5).HasError());
            CHECK(transport.Value().Read(1, {}, 1).HasError());
            CHECK(transport.Value().Read(1, slot, 5).HasError());
            CHECK(Application::CloudSaveTransport::Create(frontend, {}, *session.Subject(), access).HasError());
            CHECK(Application::CloudSaveTransport::Create(frontend, Scope(), {}, access).HasError());
            Cloud::RequireError(Application::CloudSaveTransport::Create(frontend, Scope(), *session.Subject(), {access.expected, 0}),
                                SaveErrors::NamespaceStale);
            Cloud::RequireError(Application::CloudSaveTransport::Create(frontend, Scope(), *session.Subject(), {Scope(8).localNamespace, 1}),
                                SaveErrors::NamespaceStale);
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
            const auto transport = BoundTransport(frontend, session);
            Cloud::RequireError(transport.List(1, 1), Cloud::FrontendErrors::NullProvider);
            Cloud::RequireError(transport.Read(1, Id<SaveGameSlotId>(1), 1), Cloud::FrontendErrors::NullProvider);
        }

        TEST_CASE("The durable Save journal replays the same mutation after rebinding",
                  "[unit][save][cloud-transport][journal]") {
            const Tests::OwnedTestDirectory directory{"save-cloud-replay"};
            auto backend = std::make_shared<Cloud::RoutingBackend>();
            const auto session = Cloud::Session();
            auto frontend = CloudFrontend(backend, session);
            auto transport = BoundTransport(frontend, session);
            const auto request = Cloud::TestSupport::CloudWrite();
            auto intent = UploadIntent(request);
            intent.revision = Cloud::TestSupport::CloudCommit(request).committedObject->revision;
            const auto previous = [&] {
                const auto journal = NativeJournal(directory, transport);
                auto stored = journal.Store(intent);
                REQUIRE(stored.HasValue());
                REQUIRE(journal.Store(intent).HasValue());
                auto changed = intent;
                changed.generation = Id<SlotGenerationId>(9);
                Cloud::RequireError(journal.Store(changed), FileStorageErrors::Conflict);
                const auto sent = transport.DispatchMutation(20, stored.Value(), request.bytes);
                REQUIRE(sent.HasValue());
                return std::move(stored).Value();
            }();
            REQUIRE(backend->lastCloudWrite);
            auto original = *backend->lastCloudWrite;

            const auto resumedSession = Cloud::Session({7}, {6});
            auto resumedBackend = std::make_shared<Cloud::RoutingBackend>();
            auto resumedFrontend = CloudFrontend(resumedBackend, resumedSession);
            auto resumed = BoundTransport(resumedFrontend, resumedSession, Scope(), 2);
            Cloud::RequireError(resumed.DispatchMutation(21, previous, request.bytes), SaveErrors::CloudMetadataStale);
            CHECK(resumedBackend->TotalCalls() == 0);
            const auto reopened = NativeJournal(directory, resumed);
            const auto recovered = reopened.Load(intent.mutation);
            REQUIRE(recovered.HasValue());
            REQUIRE(recovered.Value());
            const auto &restored = recovered.Value()->Intent();
            CHECK(restored.generation == intent.generation);
            CHECK(restored.parent == intent.parent);
            CHECK(restored.archive == intent.archive);
            const auto retried = resumed.DispatchMutation(21, *recovered.Value(), request.bytes);
            REQUIRE(retried.HasValue());
            REQUIRE(resumedBackend->lastCloudWrite);
            const auto &retry = *resumedBackend->lastCloudWrite;
            CHECK(retried.Value().operation == 21);
            CHECK(retry.subject == *resumedSession.Subject());
            original.subject = *resumedSession.Subject();
            CHECK(Cloud::SameCloudWriteIntent(retry, original));
        }

        TEST_CASE("Save mutation dispatch rejects mismatched scope, body and damaged journal evidence",
                  "[unit][save][cloud-transport][journal]") {
            const Tests::OwnedTestDirectory directory{"save-cloud-evidence"};
            auto backend = std::make_shared<Cloud::RoutingBackend>();
            const auto session = Cloud::Session();
            auto frontend = CloudFrontend(backend, session);
            auto transport = BoundTransport(frontend, session);
            const auto journal = NativeJournal(directory, transport);
            const auto request = Cloud::TestSupport::CloudWrite();
            auto intent = UploadIntent(request);
            const auto stored = journal.Store(intent);
            REQUIRE(stored.HasValue());

            SECTION("A different account cannot resume or dispatch this mutation") {
                auto other = BoundTransport(frontend, session, Scope(4, 5, 8));
                const auto otherJournal = NativeJournal(directory, other);
                const auto foreign = otherJournal.Load(intent.mutation);
                REQUIRE(foreign.HasValue());
                CHECK_FALSE(foreign.Value());
                Cloud::RequireError(other.DispatchMutation(1, stored.Value(), request.bytes), SaveErrors::CloudMetadataStale);
            }
            SECTION("An archive with changed contents or size never reaches the provider") {
                const auto changed = Cloud::CloudBlobOwnedBytes::Copy(std::array{std::byte{43}});
                REQUIRE(changed.HasValue());
                Cloud::RequireError(transport.DispatchMutation(1, stored.Value(), changed.Value()),
                                    Cloud::CloudObjectErrors::IntegrityMismatch);
                Cloud::RequireError(transport.DispatchMutation(1, stored.Value(), Cloud::CloudBlobOwnedBytes{}),
                                    Cloud::CloudObjectErrors::IntegrityMismatch);
                Cloud::RequireError(transport.DispatchMutation(1, stored.Value()), Cloud::CloudObjectErrors::InvalidRequest);
            }
            SECTION("Truncated records do not issue a dispatch proof") {
                std::filesystem::path record;
                for (const auto &entry : std::filesystem::recursive_directory_iterator(directory.Path() / "save-cloud")) {
                    if (entry.path().extension() == ".intent")
                        record = entry.path();
                }
                REQUIRE_FALSE(record.empty());
                std::filesystem::resize_file(record, 1);
                Cloud::RequireError(journal.Load(intent.mutation), SaveErrors::CloudMetadataInvalid);
            }
            SECTION("Malformed or cancelled writes leave their mutation unrecorded") {
                intent.mutation = Id<SaveCloudMutationId>(20);
                intent.upload.reset();
                Cloud::RequireError(journal.Store(intent), SaveErrors::CloudMetadataInvalid);
                intent.upload = Application::CloudSaveUploadEvidence{request.expectedDigest, request.bytes.Bytes().size()};
                CancellationSource cancelled;
                cancelled.RequestCancellation();
                CHECK(journal.Store(intent, cancelled.Token()).HasError());
                const auto absent = journal.Load(intent.mutation);
                REQUIRE(absent.HasValue());
                CHECK_FALSE(absent.Value());
            }
            CHECK(backend->TotalCalls() == 0);
        }

        TEST_CASE("Verified cloud upload can only confirm its exact selected local generation",
                  "[unit][save][cloud-transport][staging]") {
            SaveSlotLifecycleTest::Fixture fixture;
            const auto replaced = fixture.Import(10);
            fixture.Import(11);
            REQUIRE(fixture.owner->Execute(fixture.Copy(11, 10)).HasValue());
            const auto target = fixture.Target(10);
            const auto payload = fixture.ExportBytes(10);
            auto scope = Scope();
            scope.localNamespace = target.address.namespaceAccess.expected;
            auto backend = std::make_shared<Cloud::RoutingBackend>();
            const auto session = Cloud::Session();
            auto frontend = CloudFrontend(backend, session, payload.size());
            auto transport = BoundTransport(frontend, session, scope, fixture.Access().expectedRevision);
            const Tests::OwnedTestDirectory directory{"save-cloud-stage"};
            const auto journal = NativeJournal(directory, transport);
            const auto mutation = Id<SaveCloudMutationId>(44);
            CHECK(Application::StageCloudSaveUpload(*fixture.owner, journal, target, mutation, {}, payload.size() - 1).HasError());
            const auto absent = journal.Load(mutation);
            REQUIRE(absent.HasValue());
            CHECK_FALSE(absent.Value());
            auto staged = Application::StageCloudSaveUpload(*fixture.owner, journal, target, mutation, {}, payload.size());
            REQUIRE(staged.HasValue());
            CHECK(std::ranges::equal(staged.Value().bytes.Bytes(), payload));
            CHECK(staged.Value().mutation.Intent().generation == target.generation);
            CHECK(staged.Value().mutation.Intent().parent == replaced.entry->publication.generation);
            CHECK(backend->TotalCalls() == 0);
            const auto originalIndex = fixture.Index();
            backend->cloudWriteFailure = MakeError(Cloud::CloudObjectErrors::QuotaExceeded);
            Cloud::RequireError(transport.DispatchMutation(2, staged.Value().mutation, staged.Value().bytes),
                                Cloud::CloudObjectErrors::QuotaExceeded);
            CHECK(fixture.Index().entries == originalIndex.entries);
            backend->cloudWriteFailure.reset();
            const auto request = transport.DispatchMutation(3, staged.Value().mutation, staged.Value().bytes);
            REQUIRE(request.HasValue());
            REQUIRE(backend->lastCloudWrite);
            const auto completion = Cloud::TestSupport::CloudCommit(*backend->lastCloudWrite);
            const auto observed = ObserveWrite(*backend, request.Value().handle, completion);

            auto foreignBackend = std::make_shared<Cloud::RoutingBackend>();
            auto foreignFrontend = CloudFrontend(foreignBackend, session, payload.size());
            const auto foreignTransport = BoundTransport(foreignFrontend, session, scope, fixture.Access().expectedRevision);
            const auto other = foreignTransport.DispatchMutation(3, staged.Value().mutation, staged.Value().bytes);
            REQUIRE(other.HasValue());
            CHECK(other.Value().handle.Id() == observed.id);
            CHECK(other.Value().handle.Generation() == observed.generation);
            Cloud::RequireError(foreignTransport.PublishUploadConfirmation(*fixture.owner, fixture.Access(), staged.Value().mutation,
                                                                           other.Value(), observed),
                                Cloud::RequestErrors::Stale);

            const auto index = fixture.Index();
            const auto current = SaveCloudRevisionSnapshot::Create(index, scope, Metadata(index, scope));
            REQUIRE(current.HasValue());
            const auto confirmed = transport.ConfirmUpload(staged.Value().mutation, completion, current.Value());
            REQUIRE(confirmed.HasValue());
            CHECK(confirmed.Value().Metadata().records.front().state == SaveCloudGenerationState::Clean);
            CHECK(confirmed.Value().Metadata().records.front().lastConfirmed == mutation);
            CHECK(confirmed.Value().Metadata().records.back() == current.Value().Metadata().records.back());
            const auto duplicate = transport.ConfirmUpload(staged.Value().mutation, completion, confirmed.Value());
            REQUIRE(duplicate.HasValue());
            CHECK(duplicate.Value().Metadata().revision == confirmed.Value().Metadata().revision);
            REQUIRE(transport.PublishUploadConfirmation(*fixture.owner, fixture.Access(), staged.Value().mutation, request.Value(), observed)
                        .HasValue());
            fixture.Reopen();
            const auto durable = fixture.owner->ReadCloudRevision(fixture.Access(), scope);
            REQUIRE(durable.HasValue());
            CHECK(durable.Value().Metadata() == confirmed.Value().Metadata());
            REQUIRE(transport.PublishUploadConfirmation(*fixture.owner, fixture.Access(), staged.Value().mutation, request.Value(), observed)
                        .HasValue());

            auto superseding = confirmed.Value().Metadata();
            superseding.records.front().lastConfirmed = Id<SaveCloudMutationId>(45);
            superseding.records.front().object->revision = std::vector{std::byte{8}};
            const auto superseded = SaveCloudRevisionSnapshot::Create(index, scope, std::move(superseding));
            REQUIRE(superseded.HasValue());
            Cloud::RequireError(transport.ConfirmUpload(staged.Value().mutation, completion, superseded.Value()),
                                SaveErrors::CloudMetadataStale);

            auto altered = completion;
            altered.committedObject->sizeBytes += 1;
            Cloud::RequireError(transport.ConfirmUpload(staged.Value().mutation, altered, current.Value()),
                                Cloud::CloudObjectErrors::InvalidProviderResponse);
            REQUIRE(fixture.owner->Execute(fixture.Copy(11, 10)).HasValue());
            const auto advanced = fixture.Index();
            const auto fresh = SaveCloudRevisionSnapshot::Create(advanced, scope, Metadata(advanced, scope));
            REQUIRE(fresh.HasValue());
            Cloud::RequireError(transport.ConfirmUpload(staged.Value().mutation, completion, fresh.Value()), SaveErrors::CloudMetadataStale);
            Cloud::RequireError(transport.PublishUploadConfirmation(*fixture.owner, fixture.Access(), staged.Value().mutation, request.Value(),
                                                                    observed),
                                SaveErrors::CloudMetadataStale);
            CHECK(fixture.Index().entries == advanced.entries);
            REQUIRE(frontend.Close().HasValue());
            Cloud::RequireError(transport.ConfirmUpload(staged.Value().mutation, completion, current.Value()),
                                SaveErrors::CloudMetadataStale);
        }

        TEST_CASE("Uncertain uploads recover from the exact bounded provider read",
                  "[unit][save][cloud-transport][recovery]") {
            SaveSlotLifecycleTest::Fixture fixture;
            fixture.Import(10);
            fixture.Import(11);
            const auto target = fixture.Target(10);
            const auto payload = fixture.ExportBytes(10);
            auto scope = Scope();
            scope.localNamespace = fixture.Access().expected;
            auto backend = std::make_shared<Cloud::RoutingBackend>();
            const auto session = Cloud::Session();
            auto frontend = CloudFrontend(backend, session, payload.size());
            auto transport = BoundTransport(frontend, session, scope, fixture.Access().expectedRevision);
            const Tests::OwnedTestDirectory directory{"save-cloud-recovery"};
            const auto journal = NativeJournal(directory, transport);
            const auto staged = Application::StageCloudSaveUpload(
                *fixture.owner, journal, target, Id<SaveCloudMutationId>(44), {}, payload.size());
            REQUIRE(staged.HasValue());
            REQUIRE(transport.DispatchMutation(2, staged.Value().mutation, staged.Value().bytes).HasValue());
            REQUIRE(backend->lastCloudWrite);
            const auto write = *backend->lastCloudWrite;
            const auto probe = transport.ProbeUpload(2, staged.Value().mutation);
            REQUIRE(probe.HasValue());
            const auto &handle = probe.Value().Request().handle;
            REQUIRE(backend->lastCloudRead);
            CHECK(backend->lastCloudRead->key == write.key);
            CHECK(backend->lastCloudRead->maximumBytes == payload.size());
            const auto before = fixture.owner->ReadCloudRevision(fixture.Access(), scope);
            REQUIRE(before.HasValue());
            REQUIRE(backend->requests.MarkRunning(handle).HasValue());

            const auto recover = [&](const CancellationToken &cancellation = CancellationToken{}) {
                const auto snapshot = backend->requests.Query(handle);
                REQUIRE(snapshot.HasValue());
                return transport.ReconcileUpload(*fixture.owner, fixture.Access(), probe.Value(), snapshot.Value(), cancellation);
            };

            SECTION("A commit survives loss of the write response and restart of local storage") {
                auto head = *Cloud::TestSupport::CloudCommit(write).committedObject;
                head.transportDigest.reset();
                REQUIRE(backend->requests.CompleteSuccess(handle.Id(), handle.Generation(),
                            Cloud::CloudBlobReadResult{write.subject, write.subject.SessionGeneration(), head, write.bytes}).HasValue());
                bool confirmed{};
                auto subscription = transport.OnComplete(probe.Value().Request(), [&](const OperationId operation, const auto &snapshot) {
                    CHECK(operation == 2);
                    const auto result = transport.ReconcileUpload(*fixture.owner, fixture.Access(), probe.Value(), snapshot);
                    REQUIRE(result.HasValue());
                    confirmed = !result.Value().retry;
                });
                REQUIRE(subscription.HasValue());
                CHECK_FALSE(confirmed);
                CHECK(backend->requests.DispatchCompletions() == 1);
                CHECK(confirmed);
                fixture.Reopen();
                const auto stored = fixture.owner->ReadCloudRevision(fixture.Access(), scope);
                REQUIRE(stored.HasValue());
                CHECK(stored.Value().Metadata().records.front().lastConfirmed == staged.Value().mutation.Intent().mutation);
                CHECK(stored.Value().Metadata().records.front().state == SaveCloudGenerationState::Clean);
                CHECK(stored.Value().Index().entries == before.Value().Index().entries);
                const auto repeated = recover();
                REQUIRE(repeated.HasValue());
                CHECK_FALSE(repeated.Value().retry);
                CHECK(fixture.owner->ReadCloudRevision(fixture.Access(), scope).Value().Metadata() == stored.Value().Metadata());
            }
            SECTION("Only exact absence permits retry of the original conditional create") {
                REQUIRE(backend->requests.CompleteFailure(handle, MakeError(Cloud::CloudObjectErrors::NotFound)).HasValue());
                fixture.Reopen();
                CancellationSource stop;
                stop.RequestCancellation();
                Cloud::RequireError(recover(stop.Token()), SaveErrors::OperationCancelled);
                const auto recovered = recover();
                REQUIRE(recovered.HasValue());
                REQUIRE(recovered.Value().retry);
                const auto &retry = *recovered.Value().retry;
                CHECK(retry.mutation.Intent().mutation == staged.Value().mutation.Intent().mutation);
                CHECK(std::ranges::equal(retry.bytes.Bytes(), payload));
                CHECK(fixture.owner->ReadCloudRevision(fixture.Access(), scope).Value().Metadata() == before.Value().Metadata());
                REQUIRE(transport.DispatchMutation(probe.Value().Request().operation, retry.mutation, retry.bytes).HasValue());
                CHECK(backend->lastCloudWrite->mutation == write.mutation);
                CHECK(std::holds_alternative<Cloud::CloudCreateIfAbsent>(backend->lastCloudWrite->precondition));
                REQUIRE(fixture.owner->Execute(fixture.Copy(11, 10)).HasValue());
                Cloud::RequireError(recover(), SaveErrors::CloudMetadataStale);
            }
            SECTION("An unrelated request's missing result cannot permit a retry") {
                const auto unrelated = transport.Read(4, staged.Value().mutation.Intent().slot, payload.size());
                REQUIRE(unrelated.HasValue());
                const auto &other = unrelated.Value().handle;
                REQUIRE(backend->requests.MarkRunning(other).HasValue());
                REQUIRE(backend->requests.CompleteFailure(other, MakeError(Cloud::CloudObjectErrors::NotFound)).HasValue());
                const auto snapshot = backend->requests.Query(other);
                REQUIRE(snapshot.HasValue());
                Cloud::RequireError(transport.ReconcileUpload(*fixture.owner, fixture.Access(), probe.Value(), snapshot.Value()),
                                    Cloud::RequestErrors::Stale);
            }
            SECTION("A different complete object is a normal remote conflict") {
                auto otherBytes = payload;
                otherBytes.front() ^= std::byte{1};
                auto bytes = Cloud::CloudBlobOwnedBytes::Copy(otherBytes);
                REQUIRE(bytes.HasValue());
                auto head = *Cloud::TestSupport::CloudCommit(write).committedObject;
                head.transportDigest = ComputeSha256(otherBytes);
                REQUIRE(backend->requests.CompleteSuccess(handle.Id(), handle.Generation(),
                            Cloud::CloudBlobReadResult{write.subject, write.subject.SessionGeneration(), head, bytes.Value()}).HasValue());
                Cloud::RequireError(recover(), Cloud::CloudObjectErrors::PreconditionFailed);
                CHECK(fixture.owner->ReadCloudRevision(fixture.Access(), scope).Value().Metadata() == before.Value().Metadata());
            }
            SECTION("An incomplete or failed read leaves the outcome unknown") {
                Cloud::RequireError(recover(), Cloud::CloudObjectErrors::InvalidRequest);
                REQUIRE(backend->requests.CompleteFailure(handle, MakeError(Cloud::CloudObjectErrors::PartialTransfer)).HasValue());
                Cloud::RequireError(recover(), Cloud::CloudObjectErrors::PartialTransfer);
                CHECK(fixture.owner->ReadCloudRevision(fixture.Access(), scope).Value().Metadata() == before.Value().Metadata());
            }
        }

        TEST_CASE("An uncertain replacement does not confirm the original remote revision",
                  "[unit][save][cloud-transport][recovery]") {
            SaveSlotLifecycleTest::Fixture fixture;
            fixture.Import(10);
            const auto target = fixture.Target(10);
            const auto payload = fixture.ExportBytes(10);
            auto scope = Scope();
            scope.localNamespace = fixture.Access().expected;
            auto backend = std::make_shared<Cloud::RoutingBackend>();
            const auto session = Cloud::Session();
            auto frontend = CloudFrontend(backend, session, payload.size());
            auto transport = BoundTransport(frontend, session, scope, fixture.Access().expectedRevision);
            const Tests::OwnedTestDirectory directory{"save-cloud-replace-recovery"};
            const auto journal = NativeJournal(directory, transport);
            const auto initial = Application::StageCloudSaveUpload(
                *fixture.owner, journal, target, Id<SaveCloudMutationId>(44), {}, payload.size());
            REQUIRE(initial.HasValue());
            const auto request = transport.DispatchMutation(2, initial.Value().mutation, initial.Value().bytes);
            REQUIRE(request.HasValue());
            REQUIRE(backend->lastCloudWrite);
            const auto committed = Cloud::TestSupport::CloudCommit(*backend->lastCloudWrite);
            const auto observed = ObserveWrite(*backend, request.Value().handle, committed);
            REQUIRE(transport.PublishUploadConfirmation(*fixture.owner, fixture.Access(), initial.Value().mutation, request.Value(), observed)
                        .HasValue());
            auto replacementIntent = initial.Value().mutation.Intent();
            replacementIntent.mutation = Id<SaveCloudMutationId>(45);
            replacementIntent.revision = committed.committedObject->revision;
            const auto replacement = journal.Store(replacementIntent);
            REQUIRE(replacement.HasValue());
            const auto probe = transport.ProbeUpload(3, replacement.Value());
            REQUIRE(probe.HasValue());
            const auto &handle = probe.Value().Request().handle;
            REQUIRE(backend->requests.MarkRunning(handle).HasValue());

            SECTION("The old revision with identical bytes permits only the original replace") {
                REQUIRE(backend->requests.CompleteSuccess(handle.Id(), handle.Generation(),
                            Cloud::CloudBlobReadResult{committed.subject, committed.sessionGeneration,
                                                       *committed.committedObject, initial.Value().bytes}).HasValue());
                const auto snapshot = backend->requests.Query(handle);
                REQUIRE(snapshot.HasValue());
                const auto recovery = transport.ReconcileUpload(*fixture.owner, fixture.Access(), probe.Value(), snapshot.Value());
                REQUIRE(recovery.HasValue());
                REQUIRE(recovery.Value().retry);
                CHECK(recovery.Value().retry->mutation.Intent().revision == committed.committedObject->revision);
                CHECK(std::ranges::equal(recovery.Value().retry->bytes.Bytes(), payload));
            }
            SECTION("Missing is a conflict for a replacement") {
                REQUIRE(backend->requests.CompleteFailure(handle, MakeError(Cloud::CloudObjectErrors::NotFound)).HasValue());
                const auto snapshot = backend->requests.Query(handle);
                REQUIRE(snapshot.HasValue());
                Cloud::RequireError(transport.ReconcileUpload(*fixture.owner, fixture.Access(), probe.Value(), snapshot.Value()),
                                    Cloud::CloudObjectErrors::PreconditionFailed);
            }
            const auto stored = fixture.owner->ReadCloudRevision(fixture.Access(), scope);
            REQUIRE(stored.HasValue());
            CHECK(stored.Value().Metadata().records.front().lastConfirmed == initial.Value().mutation.Intent().mutation);
        }

        TEST_CASE("A selected cloud archive imports through the local catalog transaction",
                  "[unit][save][cloud-transport][download]") {
            SaveSlotLifecycleTest::Fixture fixture;
            const auto existing = fixture.Import(10);
            const auto payload = fixture.ExportBytes(10);
            const auto before = fixture.Index();
            const Application::CloudSaveImportTarget target{fixture.Target(12), 0, {"Downloaded"}};
            auto scope = Scope();
            scope.localNamespace = fixture.Access().expected;
            auto backend = std::make_shared<Cloud::RoutingBackend>();
            const auto session = Cloud::Session();
            auto frontend = CloudFrontend(backend, session, payload.size() + 128);
            const auto transport = BoundTransport(frontend, session, scope, fixture.Access().expectedRevision);
            const auto revision = Cloud::ProviderObjectRevision::Copy(std::array{std::byte{3}});
            REQUIRE(revision.HasValue());
            const auto remoteSlot = existing.entry->publication.slot;
            const auto download = transport.DownloadRevision(31, remoteSlot, revision.Value(), payload.size() + 128);
            REQUIRE(download.HasValue());
            REQUIRE(backend->lastCloudRead);
            const auto &handle = download.Value().Request().handle;
            REQUIRE(backend->requests.MarkRunning(handle).HasValue());
            const auto subject = *session.Subject();
            const auto completeRead = [&](const auto &readHandle, const std::vector<std::byte> &archive,
                                          const Cloud::ProviderObjectRevision &observed) {
                const auto bytes = Cloud::CloudBlobOwnedBytes::Copy(archive);
                REQUIRE(bytes.HasValue());
                const Cloud::CloudObjectHead head{backend->lastCloudRead->key, observed, archive.size(), ComputeSha256(archive)};
                REQUIRE(backend->requests.CompleteSuccess(readHandle.Id(), readHandle.Generation(),
                            Cloud::CloudBlobReadResult{subject, subject.SessionGeneration(), head, bytes.Value()}).HasValue());
                auto snapshot = backend->requests.Query(readHandle);
                REQUIRE(snapshot.HasValue());
                return std::move(snapshot).Value();
            };
            const auto complete = [&](const std::vector<std::byte> &archive, const Cloud::ProviderObjectRevision &observed) {
                return completeRead(handle, archive, observed);
            };

            SECTION("The approved revision creates a new generation in an absent slot") {
                std::optional<SaveSlotLifecycleResult> published;
                auto watched = transport.OnComplete(download.Value().Request(), [&](const OperationId operation, const auto &snapshot) {
                    CHECK(operation == 31);
                    auto result = transport.ImportDownload(*fixture.owner, download.Value(), snapshot, target);
                    REQUIRE(result.HasValue());
                    published = std::move(result).Value();
                });
                REQUIRE(watched.HasValue());
                const auto snapshot = complete(payload, revision.Value());
                CHECK_FALSE(published);
                CHECK(backend->requests.DispatchCompletions() == 1);
                REQUIRE(published);
                REQUIRE(published->entry);
                CHECK(published->entry->publication.slot == target.destination.address.slot);
                CHECK(published->entry->publication.generation != existing.entry->publication.generation);
                fixture.Reopen();
                const auto after = fixture.Index();
                REQUIRE(after.entries.size() == 2);
                CHECK(after.entries.front() == before.entries.front());
                CHECK(after.entries.back() == *published->entry);
                Cloud::RequireError(transport.ImportDownload(*fixture.owner, download.Value(), snapshot, target),
                                    SaveErrors::CloudMetadataStale);
            }
            SECTION("An updated remote revision cannot replace the selected one") {
                const auto other = Cloud::ProviderObjectRevision::Copy(std::array{std::byte{4}});
                REQUIRE(other.HasValue());
                const auto snapshot = complete(payload, other.Value());
                Cloud::RequireError(transport.ImportDownload(*fixture.owner, download.Value(), snapshot, target),
                                    Cloud::CloudObjectErrors::RevisionChanged);
                CHECK(fixture.Index().entries == before.entries);
            }
            SECTION("A replacement frontend cannot consume a queued result from the former issuing store") {
                const auto old = complete(payload, revision.Value());
                REQUIRE(frontend.Close().HasValue());
                auto replacementBackend = std::make_shared<Cloud::RoutingBackend>();
                auto replacementFrontend = CloudFrontend(replacementBackend, session, payload.size() + 128);
                const auto replacement = BoundTransport(replacementFrontend, session, scope, fixture.Access().expectedRevision);
                const auto fresh = replacement.DownloadRevision(31, remoteSlot, revision.Value(), payload.size() + 128);
                REQUIRE(fresh.HasValue());
                CHECK(fresh.Value().Request().handle.Id() == old.id);
                CHECK(fresh.Value().Request().handle.Generation() == old.generation);
                Cloud::RequireError(replacement.ImportDownload(*fixture.owner, download.Value(), old, target), Cloud::RequestErrors::Stale);
                Cloud::RequireError(replacement.ImportDownload(*fixture.owner, fresh.Value(), old, target), Cloud::RequestErrors::Stale);
                CHECK(fixture.Index().entries == before.entries);
            }
            SECTION("Rebinding the same local profile invalidates old results even on the same frontend") {
                const auto old = complete(payload, revision.Value());
                CHECK(frontend.OwnsObservation(handle, old));
                ++fixture.host.binding.revision;
                const Application::CloudSaveImportTarget reboundTarget{fixture.Target(12), 0, {"Downloaded"}};
                const auto replacement = BoundTransport(frontend, session, scope, fixture.Access().expectedRevision);
                Cloud::RequireError(transport.ImportDownload(*fixture.owner, download.Value(), old, reboundTarget),
                                    SaveErrors::CloudMetadataStale);
                Cloud::RequireError(replacement.ImportDownload(*fixture.owner, download.Value(), old, reboundTarget),
                                    SaveErrors::CloudMetadataStale);
                Cloud::RequireError(transport.ImportDownload(*fixture.owner, download.Value(), old, target),
                                    SaveErrors::NamespaceStale);
                CHECK(fixture.Index().entries == before.entries);

                const auto fresh = replacement.DownloadRevision(31, remoteSlot, revision.Value(), payload.size());
                REQUIRE(fresh.HasValue());
                const auto &freshHandle = fresh.Value().Request().handle;
                REQUIRE(backend->requests.MarkRunning(freshHandle).HasValue());
                const auto current = completeRead(freshHandle, payload, revision.Value());
                REQUIRE(replacement.ImportDownload(*fixture.owner, fresh.Value(), current, reboundTarget).HasValue());
            }
            SECTION("A valid archive belonging to a different slot remains isolated") {
                const auto archive = SaveSlotLifecycleTest::ReadOwned(payload);
                auto header = archive.Header();
                header.slot = Id<SaveGameSlotId>(11);
                const auto wrongSlot = SaveSlotLifecycleTest::RewriteHeader(archive, header);
                const auto snapshot = complete(wrongSlot, revision.Value());
                Cloud::RequireError(transport.ImportDownload(*fixture.owner, download.Value(), snapshot, target),
                                    SaveErrors::StoragePermissionDenied);
                CHECK(std::ranges::equal(snapshot.terminal->Value()->bytes.Bytes(), wrongSlot));
                CHECK(fixture.Index().entries == before.entries);
            }
            SECTION("An explicitly configured import source cannot change the cloud profile") {
                fixture.owner.reset();
                auto foreign = fixture.policy.destination;
                foreign.profile = Id<GameProfileId>(90);
                foreign.name.owner = UserProfileOwner{foreign.user, foreign.profile};
                fixture.policy.importSources.push_back(foreign);
                fixture.Open();
                const auto archive = SaveSlotLifecycleTest::ReadOwned(payload);
                auto header = archive.Header();
                header.profile = foreign.profile;
                const auto snapshot = complete(SaveSlotLifecycleTest::RewriteHeader(archive, header), revision.Value());
                auto foreignTarget = target;
                foreignTarget.sourceScope = 1;
                Cloud::RequireError(transport.ImportDownload(*fixture.owner, download.Value(), snapshot, foreignTarget),
                                    SaveErrors::StoragePermissionDenied);
                CHECK(fixture.Index().entries == before.entries);
            }
            SECTION("Cancellation and occupied destinations leave the catalog intact") {
                const auto snapshot = complete(payload, revision.Value());
                CancellationSource stop;
                stop.RequestCancellation();
                Cloud::RequireError(transport.ImportDownload(*fixture.owner, download.Value(), snapshot, target, stop.Token()),
                                    SaveErrors::OperationCancelled);
                const Application::CloudSaveImportTarget occupied{fixture.Target(10), 0, {"Downloaded"}};
                Cloud::RequireError(transport.ImportDownload(*fixture.owner, download.Value(), snapshot, occupied),
                                    SaveErrors::StoragePermissionDenied);
                CHECK(fixture.Index().entries == before.entries);
            }
            SECTION("Invalid archive bytes remain available to the operation and never enter the catalog") {
                auto damaged = payload;
                damaged.front() ^= std::byte{1};
                const auto snapshot = complete(damaged, revision.Value());
                CHECK(transport.ImportDownload(*fixture.owner, download.Value(), snapshot, target).HasError());
                CHECK(std::ranges::equal(snapshot.terminal->Value()->bytes.Bytes(), damaged));
                CHECK(fixture.Index().entries == before.entries);
            }
            SECTION("Quarantine retains one private complete observation and never replaces it at capacity") {
                const Tests::OwnedTestDirectory directory{"save-cloud-quarantine"};
                auto storage = Platform::CreateNativeImmutableFileStorage(directory.Path(), "rejected");
                REQUIRE(storage.HasValue());
                const Application::CloudSaveQuarantinePolicy policy{*storage.Value(), payload.size(), 1};
                auto damaged = payload;
                damaged.front() ^= std::byte{1};
                const auto snapshot = complete(damaged, revision.Value());
                const auto failure = transport.ImportDownload(*fixture.owner, download.Value(), snapshot, target, {}, &policy);
                REQUIRE(failure.HasError());
                CHECK_FALSE(failure.ErrorValue().cause);
                const auto replay = transport.ImportDownload(*fixture.owner, download.Value(), snapshot, target, {}, &policy);
                REQUIRE(replay.HasError());
                CHECK_FALSE(replay.ErrorValue().cause);
                std::vector<std::filesystem::path> retained;
                for (const auto &entry : std::filesystem::recursive_directory_iterator(directory.Path() / "rejected")) {
                    if (entry.is_regular_file())
                        retained.push_back(entry.path());
                }
                REQUIRE(retained.size() == 1);
                const auto evidence = SaveSlotLifecycleTest::DiskBytes(retained.front());
                REQUIRE(evidence.size() > damaged.size());
                CHECK(std::ranges::equal(std::span{evidence}.last(damaged.size()), damaged));

                const auto nextRevision = Cloud::ProviderObjectRevision::Copy(std::array{std::byte{4}});
                REQUIRE(nextRevision.HasValue());
                const auto second = transport.DownloadRevision(32, remoteSlot, nextRevision.Value(), payload.size());
                REQUIRE(second.HasValue());
                const auto &secondHandle = second.Value().Request().handle;
                REQUIRE(backend->requests.MarkRunning(secondHandle).HasValue());
                const auto different = completeRead(secondHandle, damaged, nextRevision.Value());
                const auto full = transport.ImportDownload(*fixture.owner, second.Value(), different, target, {}, &policy);
                REQUIRE(full.HasError());
                CHECK(full.ErrorValue().code.Value() == failure.ErrorValue().code.Value());
                CHECK(ErrorChainContains(full.ErrorValue(), FileStorageErrors::Capacity.domain, FileStorageErrors::Capacity.code));
                CHECK(SaveSlotLifecycleTest::DiskBytes(retained.front()) == evidence);
                fixture.Reopen();
                CHECK(fixture.Index().entries == before.entries);
            }
        }

        TEST_CASE("A cloud successor preserves its generation and publishes paired remote evidence",
                  "[unit][save][cloud-transport][download]") {
            SaveSlotLifecycleTest::Fixture fixture;
            fixture.Import(10);
            fixture.Import(11);
            const auto original = fixture.ExportBytes(10);
            const auto target = fixture.Target(10);
            auto scope = Scope();
            scope.localNamespace = fixture.Access().expected;
            const auto baseline = fixture.owner->ReadCloudRevision(fixture.Access(), scope);
            REQUIRE(baseline.HasValue());
            auto inFlight = baseline.Value().Metadata();
            ++inFlight.revision;
            inFlight.records.back().state = SaveCloudGenerationState::Uploading;
            const auto uploading = SaveCloudRevisionSnapshot::Create(baseline.Value().Index(), scope, inFlight);
            REQUIRE(uploading.HasValue());
            REQUIRE(fixture.owner->PublishCloudRevision(fixture.Access(), baseline.Value(), uploading.Value()).HasValue());
            const auto expected = fixture.owner->ReadCloudRevision(fixture.Access(), scope);
            REQUIRE(expected.HasValue());
            const auto archive = SaveSlotLifecycleTest::ReadOwned(original);
            auto header = archive.Header();
            header.slotGeneration = Id<SlotGenerationId>(90);
            header.parentGeneration = target.generation;
            const auto candidate = SaveSlotLifecycleTest::RewriteHeader(archive, header);
            auto backend = std::make_shared<Cloud::RoutingBackend>();
            const auto session = Cloud::Session();
            auto frontend = CloudFrontend(backend, session, candidate.size() + 128);
            const auto transport = BoundTransport(frontend, session, scope, fixture.Access().expectedRevision);
            const auto revision = Cloud::ProviderObjectRevision::Copy(std::array{std::byte{8}});
            REQUIRE(revision.HasValue());
            const auto download = transport.DownloadRevision(32, target.address.slot, revision.Value(), candidate.size() + 128);
            REQUIRE(download.HasValue());
            REQUIRE(backend->lastCloudRead);
            const auto &handle = download.Value().Request().handle;
            REQUIRE(backend->requests.MarkRunning(handle).HasValue());
            const auto subject = *session.Subject();
            const auto complete = [&](const std::vector<std::byte> &payload, const Application::CloudSaveQuarantinePolicy *policy = nullptr) {
                auto bytes = Cloud::CloudBlobOwnedBytes::Copy(payload);
                REQUIRE(bytes.HasValue());
                const Cloud::CloudObjectHead head{backend->lastCloudRead->key, revision.Value(), payload.size(), ComputeSha256(payload)};
                REQUIRE(backend->requests.CompleteSuccess(handle.Id(), handle.Generation(),
                            Cloud::CloudBlobReadResult{subject, subject.SessionGeneration(), head, std::move(bytes).Value()}).HasValue());
                const auto snapshot = backend->requests.Query(handle);
                REQUIRE(snapshot.HasValue());
                return transport.ApplyDownload(*fixture.owner, download.Value(), snapshot.Value(), target, expected.Value(), {}, policy);
            };
            const auto checkPublished = [&] {
                fixture.Reopen();
                CHECK(fixture.ExportBytes(10) == candidate);
                CHECK(fixture.Target(10).generation == header.slotGeneration);
                const auto pair = fixture.owner->ReadCloudRevision(fixture.Access(), scope);
                REQUIRE(pair.HasValue());
                REQUIRE(pair.Value().Metadata().records.size() == 2);
                CHECK(pair.Value().Metadata().records.back() == inFlight.records.back());
                const auto &record = pair.Value().Metadata().records.front();
                CHECK(record.generation == header.slotGeneration);
                CHECK(record.state == SaveCloudGenerationState::Clean);
                REQUIRE(record.object);
                REQUIRE(record.object->revision);
                CHECK(std::ranges::equal(record.object->revision.value(), revision.Value().Bytes()));
            };

            SECTION("The exact remote archive is selected atomically") {
                const auto applied = complete(candidate);
                REQUIRE(applied.HasValue());
                REQUIRE(applied.Value().entry);
                CHECK(applied.Value().entry->publication.generation == header.slotGeneration);
                checkPublished();
            }
            SECTION("A divergent archive leaves both observations intact") {
                auto divergent = header;
                divergent.parentGeneration = Id<SlotGenerationId>(91);
                Cloud::RequireError(complete(SaveSlotLifecycleTest::RewriteHeader(archive, divergent)),
                                    SaveErrors::SlotCommitGenerationStale);
                CHECK(fixture.ExportBytes(10) == original);
                CHECK(fixture.owner->ReadCloudRevision(fixture.Access(), scope).Value().Metadata() == expected.Value().Metadata());
            }
            SECTION("Reusing the selected generation for different bytes is an integrity rejection") {
                auto reused = archive.Header();
                ++reused.capturedAtUnixMilliseconds;
                const auto changed = SaveSlotLifecycleTest::RewriteHeader(archive, reused);
                const auto rejected = complete(changed);
                Cloud::RequireError(rejected, SaveErrors::CloudArchiveRejected);
                CHECK(ErrorChainContains(rejected.ErrorValue(), SaveErrors::ArchiveContentHashMismatch.domain,
                                         SaveErrors::ArchiveContentHashMismatch.code));
                CHECK(fixture.ExportBytes(10) == original);
            }
            SECTION("A concurrent local save invalidates the earlier selection") {
                fixture.Import(10);
                const auto newer = fixture.ExportBytes(10);
                Cloud::RequireError(complete(candidate), SaveErrors::CloudMetadataStale);
                CHECK(fixture.ExportBytes(10) == newer);
            }
            SECTION("A failed catalog replace keeps the old pair") {
                const Tests::OwnedTestDirectory directory{"save-cloud-apply-publication"};
                auto storage = Platform::CreateNativeImmutableFileStorage(directory.Path(), "evidence");
                REQUIRE(storage.HasValue());
                const Application::CloudSaveQuarantinePolicy policy{*storage.Value(), candidate.size(), 1};
                fixture.InjectFailure(SaveSlotLifecycleIoStage::Replace, SaveSlotLifecycleFileKind::Catalog);
                const auto failure = complete(candidate, &policy);
                REQUIRE(failure.HasError());
                CHECK(failure.ErrorValue().code.Value() != SaveErrors::CloudArchiveRejected.code.Value());
                CHECK_FALSE(std::filesystem::exists(directory.Path() / "evidence" / "quarantine"));
                fixture.fault.enabled = false;
                fixture.Reopen();
                CHECK(fixture.ExportBytes(10) == original);
                CHECK(fixture.owner->ReadCloudRevision(fixture.Access(), scope).Value().Metadata() == expected.Value().Metadata());
            }
            SECTION("A malformed same-slot download is quarantined without touching the selected pair") {
                const Tests::OwnedTestDirectory directory{"save-cloud-apply-rejected"};
                auto storage = Platform::CreateNativeImmutableFileStorage(directory.Path(), "evidence");
                REQUIRE(storage.HasValue());
                const Application::CloudSaveQuarantinePolicy policy{*storage.Value(), candidate.size(), 1};
                auto damaged = candidate;
                damaged.front() ^= std::byte{1};
                const auto rejected = complete(damaged, &policy);
                Cloud::RequireError(rejected, SaveErrors::CloudArchiveRejected);
                REQUIRE(rejected.ErrorValue().cause);
                std::size_t count{};
                for (const auto &entry : std::filesystem::recursive_directory_iterator(directory.Path() / "evidence")) {
                    if (entry.is_regular_file()) {
                        const auto bytes = SaveSlotLifecycleTest::DiskBytes(entry.path());
                        REQUIRE(bytes.size() > damaged.size());
                        CHECK(std::ranges::equal(std::span{bytes}.last(damaged.size()), damaged));
                        ++count;
                    }
                }
                CHECK(count == 1);
                fixture.Reopen();
                CHECK(fixture.ExportBytes(10) == original);
                CHECK(fixture.owner->ReadCloudRevision(fixture.Access(), scope).Value().Metadata() == expected.Value().Metadata());
            }
            SECTION("Recovery keeps the new archive and remote revision paired after visibility") {
                fixture.InjectFailure(SaveSlotLifecycleIoStage::DirectorySync, SaveSlotLifecycleFileKind::Catalog);
                Cloud::RequireError(complete(candidate), SaveErrors::SlotCommitOutcomeUnknown);
                fixture.fault.enabled = false;
                checkPublished();
            }
        }

        TEST_CASE("Cloud catalog publication preserves exact pairing across reload and local slot changes",
                  "[unit][save][cloud-transport][catalog]") {
            SaveSlotLifecycleTest::Fixture fixture;
            fixture.Import(10);
            fixture.Import(11);
            auto scope = Scope();
            scope.localNamespace = fixture.Access().expected;
            const auto initial = fixture.owner->ReadCloudRevision(fixture.Access(), scope);
            REQUIRE(initial.HasValue());
            CHECK(initial.Value().Metadata().revision == 1);
            CHECK(initial.Value().Metadata().records.front().state == SaveCloudGenerationState::Unknown);
            auto proposed = initial.Value().Metadata();
            ++proposed.revision;
            proposed.records.front().state = SaveCloudGenerationState::Clean;
            proposed.records.front().object = Object();
            proposed.records.front().lastConfirmed = Id<SaveCloudMutationId>(45);
            const auto candidate = SaveCloudRevisionSnapshot::Create(initial.Value().Index(), scope, proposed);
            REQUIRE(candidate.HasValue());

            SECTION("The old pair survives pre-visibility failure") {
                fixture.InjectFailure(SaveSlotLifecycleIoStage::Replace, SaveSlotLifecycleFileKind::Catalog);
                CHECK(fixture.owner->PublishCloudRevision(fixture.Access(), initial.Value(), candidate.Value()).HasError());
                fixture.fault.enabled = false;
                fixture.Reopen();
                const auto restored = fixture.owner->ReadCloudRevision(fixture.Access(), scope);
                REQUIRE(restored.HasValue());
                CHECK(restored.Value().Metadata() == initial.Value().Metadata());
            }
            SECTION("A visible replacement with failed directory sync is recoverable") {
                fixture.InjectFailure(SaveSlotLifecycleIoStage::DirectorySync, SaveSlotLifecycleFileKind::Catalog);
                Cloud::RequireError(fixture.owner->PublishCloudRevision(fixture.Access(), initial.Value(), candidate.Value()),
                                    SaveErrors::SlotCommitOutcomeUnknown);
                fixture.fault.enabled = false;
                fixture.Reopen();
                const auto restored = fixture.owner->ReadCloudRevision(fixture.Access(), scope);
                REQUIRE(restored.HasValue());
                CHECK(restored.Value().Metadata() == proposed);
                CHECK(fixture.owner->PublishCloudRevision(fixture.Access(), restored.Value(), candidate.Value()).HasValue());
            }
            SECTION("A newer catalog keeps only unchanged generation evidence") {
                REQUIRE(fixture.owner->PublishCloudRevision(fixture.Access(), initial.Value(), candidate.Value()).HasValue());
                Cloud::RequireError(fixture.owner->PublishCloudRevision(fixture.Access(), initial.Value(), candidate.Value()),
                                    SaveErrors::CloudMetadataStale);
                REQUIRE(fixture.owner->Execute(fixture.Copy(10, 11)).HasValue());
                fixture.Reopen();
                const auto newer = fixture.owner->ReadCloudRevision(fixture.Access(), scope);
                REQUIRE(newer.HasValue());
                CHECK(newer.Value().Metadata().indexRevision == newer.Value().Index().revision);
                CHECK(newer.Value().Metadata().records.front() == proposed.records.front());
                REQUIRE(fixture.owner->Execute(fixture.Copy(11, 10)).HasValue());
                fixture.Reopen();
                const auto replaced = fixture.owner->ReadCloudRevision(fixture.Access(), scope);
                REQUIRE(replaced.HasValue());
                CHECK(replaced.Value().Metadata().records.front().state == SaveCloudGenerationState::Unknown);
                CHECK_FALSE(replaced.Value().Metadata().records.front().lastConfirmed);
                auto different = scope;
                different.account = Id<SaveCloudAccountId>(16);
                Cloud::RequireError(fixture.owner->ReadCloudRevision(fixture.Access(), different), SaveErrors::CloudMetadataStale);
            }
            SECTION("Storage binding alone does not grant cloud publication") {
                fixture.policy.capabilities &= ~(1U << static_cast<unsigned>(SaveSlotLifecycleKind::CloudSync));
                fixture.Reopen();
                Cloud::RequireError(fixture.owner->ReadCloudRevision(fixture.Access(), scope), SaveErrors::StoragePermissionDenied);
                Cloud::RequireError(fixture.owner->PublishCloudRevision(fixture.Access(), initial.Value(), candidate.Value()),
                                    SaveErrors::StoragePermissionDenied);
            }
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
