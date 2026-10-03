#include "Horo/Gameplay/SaveGameplayPersistence.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveParticipation.h"
#include "SaveGameplayPersistenceTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <exception>
#include <memory>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        using namespace Test::GameplayPersistence;

        /** @brief Test project failure using the standard exception hierarchy. */
        class ProjectCallbackFailure final : public std::exception {
        public:
            const char *what() const noexcept override {
                return "project persistence callback failed";
            }
        };

        struct ForeignProjectCallbackFailure final {};

        enum class ProjectCallbackException {
            Standard,
            Integer,
            Foreign,
            Allocation
        };

        /** @brief Exercises foreign exception containment and phase-specific allocation translation. */
        [[noreturn]] void ThrowProjectCallback(const ProjectCallbackException exception) {
            switch (exception) {
                case ProjectCallbackException::Standard:
                    throw ProjectCallbackFailure{};
                case ProjectCallbackException::Integer:
                    throw 42;
                case ProjectCallbackException::Foreign:
                    throw ForeignProjectCallbackFailure{};
                case ProjectCallbackException::Allocation:
                    throw std::bad_alloc{};
            }
            std::terminate();
        }

        class PreparedState final : public IPreparedGameplayPersistenceState {
        public:
            PreparedState(std::vector<std::byte> &active, std::vector<std::byte> candidate)
                : active_(active), candidate_(std::move(candidate)) {}

            void Publish() noexcept override {
                active_.swap(candidate_);
            }

        private:
            std::vector<std::byte> &active_;
            std::vector<std::byte> candidate_;
        };

        class StateSource final : public IGameplayPersistenceSource {
        public:
            std::vector<std::byte> active{std::byte{0x31}};
            std::vector<std::byte> authoredFields{std::byte{0x7a}};
            mutable std::uint64_t observedMaximum{};
            unsigned prepareCalls{};
            bool throwOnCapture{};
            bool throwOnPrepare{};
            ProjectCallbackException exception{ProjectCallbackException::Integer};

            Result<std::vector<std::byte>> CaptureRuntimeState(const std::uint64_t maximumBytes) const override {
                observedMaximum = maximumBytes;
                if (throwOnCapture)
                    ThrowProjectCallback(exception);
                return Result<std::vector<std::byte>>::Success(active);
            }

            Result<std::unique_ptr<IPreparedGameplayPersistenceState>> PrepareRuntimeState(
                const std::span<const std::byte> bytes) override {
                ++prepareCalls;
                if (throwOnPrepare)
                    ThrowProjectCallback(exception);
                return Result<std::unique_ptr<IPreparedGameplayPersistenceState>>::Success(
                    std::make_unique<PreparedState>(active, std::vector<std::byte>{bytes.begin(), bytes.end()}));
            }
        };

        TEST_CASE("Gameplay project exception boundaries contain standard and unknown capture and prepare failures",
                  "[unit][runtime][save][gameplay]") {
            auto source = std::make_shared<StateSource>();
            auto adapter =
                GameplayPersistenceAdapter::Create(Descriptor(GameplayPersistenceOwner::Service), source, std::make_shared<int>(1)).Value();
            const auto snapshot = CaptureRegisteredState(adapter);
            for (const auto exception : {ProjectCallbackException::Standard, ProjectCallbackException::Integer,
                                         ProjectCallbackException::Foreign, ProjectCallbackException::Allocation}) {
                source->exception = exception;
                source->throwOnCapture = true;
                CanonicalStateParticipantRegistry registry;
                REQUIRE(registry.Register(adapter->Descriptor().participant, adapter).HasValue());
                const auto participants = registry.Snapshot().Value();
                auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants), participants).Value();
                const auto captured = builder.CaptureParticipants();
                REQUIRE(captured.HasError());
                const auto &captureError = exception == ProjectCallbackException::Allocation ? SaveErrors::CaptureAllocationFailed
                                                                                             : SaveErrors::LifecycleCallbackFailed;
                CHECK(captured.ErrorValue().code.Value() == captureError.code.Value());
                source->throwOnCapture = false;
                source->throwOnPrepare = true;
                auto staged = adapter->StageRestore(adapter->Descriptor().participant.schemaVersion, adapter->Descriptor().record,
                                                    snapshot.Records().front().Segment(0));
                REQUIRE(staged.HasValue());
                auto receipt = std::move(staged).Value();
                REQUIRE(receipt->Decode({}).HasValue());
                REQUIRE(receipt->Validate({}).HasValue());
                REQUIRE(receipt->Instantiate({}).HasValue());
                const NoDependencies dependencies;
                const auto prepared = receipt->ApplyState(dependencies);
                REQUIRE(prepared.HasError());
                const auto &restoreError = exception == ProjectCallbackException::Allocation ? SaveErrors::RestoreAllocationFailed
                                                                                             : SaveErrors::LifecycleCallbackFailed;
                CHECK(prepared.ErrorValue().code.Value() == restoreError.code.Value());
                CHECK(source->active == std::vector<std::byte>{std::byte{0x31}});
                CHECK(source->authoredFields == std::vector<std::byte>{std::byte{0x7a}});
                source->throwOnPrepare = false;
            }
        }

        TEST_CASE("Explicit gameplay state survives detached capture and staged publication", "[unit][runtime][save][gameplay]") {
            for (const auto owner : {GameplayPersistenceOwner::BehaviorInstance, GameplayPersistenceOwner::ModuleGlobal,
                                     GameplayPersistenceOwner::Service, GameplayPersistenceOwner::Session}) {
                auto source = std::make_shared<StateSource>();
                auto moduleLease = std::make_shared<int>(1);
                const auto descriptor = Descriptor(owner);
                auto adapter = GameplayPersistenceAdapter::Create(descriptor, source, moduleLease);
                REQUIRE(adapter.HasValue());
                CanonicalStateParticipantRegistry registry;
                REQUIRE(registry.Register(descriptor.participant, adapter.Value()).HasValue());
                const auto participants = registry.Snapshot().Value();
                auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants), participants).Value();
                REQUIRE(builder.CaptureParticipants().HasValue());
                const auto snapshot = builder.Seal().Value();
                REQUIRE(snapshot.Records().size() == 1);
                CHECK(source->observedMaximum == 95);
                CHECK(snapshot.Records().front().ByteLength() == 34);
                std::vector<std::byte> archived(snapshot.Records().front().Segment(0).begin(), snapshot.Records().front().Segment(0).end());
                CHECK(archived.back() == source->active.front());
                CHECK(archived.back() != source->authoredFields.front());

                source->active = {std::byte{0x55}};
                auto staged = adapter.Value()->StageRestore(descriptor.participant.schemaVersion, descriptor.record, archived);
                REQUIRE(staged.HasValue());
                archived.assign(archived.size(), std::byte{0xff});
                auto receipt = std::move(staged).Value();
                const NoDependencies dependencies;
                REQUIRE(receipt->Decode({}).HasValue());
                REQUIRE(receipt->Validate({}).HasValue());
                REQUIRE(receipt->Instantiate({}).HasValue());
                REQUIRE(receipt->PreparedState() != nullptr);
                REQUIRE(receipt->ApplyState(dependencies).HasValue());
                REQUIRE(receipt->FixupReferences(dependencies).HasValue());
                CHECK(source->active == std::vector<std::byte>{std::byte{0x55}});
                receipt->PublishPrepared();
                CHECK(source->active == std::vector<std::byte>{std::byte{0x31}});
                CHECK(source->authoredFields == std::vector<std::byte>{std::byte{0x7a}});
            }
        }

        TEST_CASE("Gameplay restore rejects incompatible module identity version and owner before preparation",
                  "[unit][runtime][save][gameplay][compatibility]") {
            auto source = std::make_shared<StateSource>();
            const auto descriptor = Descriptor(GameplayPersistenceOwner::Service);
            auto adapter = GameplayPersistenceAdapter::Create(descriptor, source, std::make_shared<int>(1)).Value();
            const auto snapshot = CaptureRegisteredState(adapter);
            const auto record = snapshot.Records().front().Segment(0);
            // Envelope: magic[4], identity length[1], module version[4], owner[1], module identity.
            for (const std::size_t offset : {5U, 9U, 10U}) {
                std::vector<std::byte> incompatible{record.begin(), record.end()};
                incompatible[offset] ^= std::byte{1};
                auto staged = adapter->StageRestore(descriptor.participant.schemaVersion, descriptor.record, incompatible);
                REQUIRE(staged.HasValue());
                auto receipt = std::move(staged).Value();
                REQUIRE(receipt->Decode({}).HasValue());
                const auto validated = receipt->Validate({});
                REQUIRE(validated.HasError());
                CHECK(validated.ErrorValue().code.Value() == SaveErrors::RestoreParticipantInvalid.code.Value());
                CHECK(receipt->Instantiate({}).HasError());
                CHECK(source->prepareCalls == 0);
                CHECK(source->active == std::vector<std::byte>{std::byte{0x31}});
            }
        }

        TEST_CASE("Gameplay restore rejects archive records when their module binding is absent",
                  "[unit][runtime][save][gameplay][compatibility]") {
            auto source = std::make_shared<StateSource>();
            const auto descriptor = Descriptor(GameplayPersistenceOwner::Service);
            auto adapter = GameplayPersistenceAdapter::Create(descriptor, source, std::make_shared<int>(1)).Value();
            const auto snapshot = CaptureRegisteredState(adapter);
            auto staged =
                adapter->StageRestore(descriptor.participant.schemaVersion, descriptor.record, snapshot.Records().front().Segment(0));
            REQUIRE(staged.HasValue());
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> receipts;
            receipts.push_back(std::move(staged).Value());
            CanonicalStateParticipantRegistry absentModule;
            const auto participants = absentModule.Snapshot().Value();
            auto operation =
                CreateSaveOperation({.operation = 1436, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4}).Value();
            const auto handle = operation.Handle();
            const auto created = StagedRestoreTransaction::Create({.operation = 1436,
                                                                   .registryGeneration = participants.Generation(),
                                                                   .sessionGeneration = 7,
                                                                   .sceneIncarnation = 8,
                                                                   .maximumParticipants = 1},
                                                                  std::move(operation), participants, std::move(receipts));
            REQUIRE(created.HasError());
            CHECK(created.ErrorValue().code.Value() == SaveErrors::RestoreParticipantInvalid.code.Value());
            REQUIRE(handle.Snapshot().has_value());
            CHECK(handle.Snapshot()->state == SaveOperationState::Failed);
            CHECK(source->prepareCalls == 0);
            CHECK(source->active == std::vector<std::byte>{std::byte{0x31}});
        }

        TEST_CASE("Native durable registration is inert and resolves existing gameplay owners", "[unit][runtime][save][gameplay]") {
            Gameplay::PersistenceRegistrationRegistry registrations{"game.tests"};
            auto source = std::make_shared<StateSource>();
            auto descriptor = Descriptor(GameplayPersistenceOwner::Service);
            descriptor.moduleId = SaveParticipantId::Parse("game.tests").Value();
            const auto service = Gameplay::GameplayServiceId::Parse("game.tests.missing").Value();
            CHECK(registrations.Register({descriptor, {}, source}).HasError());
            REQUIRE(registrations.Register({descriptor, service, source}).HasValue());
            CHECK(registrations.Register({descriptor, service, source}).HasError());
            CHECK(registrations.Freeze({}, {}).HasError());
            CHECK_FALSE(registrations.IsFrozen());
            CHECK(source->observedMaximum == 0);
            Gameplay::GameplayServiceRegistration existing;
            existing.descriptor.id = service;
            const std::array services{existing};
            REQUIRE(registrations.Freeze({}, services).HasValue());
            CHECK(registrations.IsFrozen());
            CHECK(registrations.Register({descriptor, service, source}).HasError());
            CHECK(source->observedMaximum == 0);
        }

        TEST_CASE("Gameplay descriptor validation retains explicit owner record scope and envelope constraints",
                  "[unit][runtime][save][gameplay]") {
            const auto valid = Descriptor(GameplayPersistenceOwner::BehaviorInstance);
            REQUIRE(IsValidGameplayPersistenceDescriptor(valid));
            std::array<GameplayPersistenceDescriptor, 9> invalid;
            invalid.fill(valid);
            invalid[0].owner = static_cast<GameplayPersistenceOwner>(255);
            invalid[1].participant.scope = SaveParticipantScope::SlotPlayer;
            invalid[2].participant.roles = SaveParticipantRole::Capture;
            invalid[3].participant.ownedRecords.clear();
            invalid[4].participant.limits.maximumRecordCount = 2;
            invalid[5].participant.limits.maximumNestingDepth = 0;
            invalid[6].participant.limits.maximumPayloadBytes = 1;
            invalid[7].moduleId = {};
            invalid[8].moduleVersion = 0;
            for (const auto &descriptor : invalid)
                CHECK_FALSE(IsValidGameplayPersistenceDescriptor(descriptor));
        }

        TEST_CASE("Gameplay persistence rejects implicit, stale and incompatible state", "[unit][runtime][save][gameplay]") {
            auto source = std::make_shared<StateSource>();
            auto lease = std::make_shared<int>(1);
            auto descriptor = Descriptor(GameplayPersistenceOwner::BehaviorInstance);
            CHECK(GameplayPersistenceAdapter::Create(descriptor, source, {}).ErrorValue().code.Value() ==
                  SaveErrors::ParticipantAdapterMissing.code.Value());
            auto invalid = descriptor;
            invalid.participant.ownedRecords.clear();
            CHECK(GameplayPersistenceAdapter::Create(invalid, source, lease).ErrorValue().code.Value() ==
                  SaveErrors::ParticipantDescriptorInvalid.code.Value());

            auto adapter = GameplayPersistenceAdapter::Create(descriptor, source, lease).Value();
            CHECK(adapter->StageRestore(Test::V<ParticipantSchemaVersion>(2), descriptor.record, {}).ErrorValue().code.Value() ==
                  SaveErrors::RestoreParticipantInvalid.code.Value());
            CHECK(adapter->StageRestore(descriptor.participant.schemaVersion, Test::Id<SaveRecordId>(2), {}).HasError());

            CanonicalStateParticipantRegistry registry;
            REQUIRE(registry.Register(descriptor.participant, adapter).HasValue());
            const auto participants = registry.Snapshot().Value();
            auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants), participants).Value();
            source->throwOnCapture = true;
            CHECK(builder.CaptureParticipants().ErrorValue().code.Value() == SaveErrors::LifecycleCallbackFailed.code.Value());
            source->throwOnCapture = false;

            auto direct = GameplayPersistenceAdapter::Create(descriptor, source, lease).Value();
            std::array<std::byte, 4> malformed{std::byte{'B'}, std::byte{'A'}, std::byte{'D'}, std::byte{'!'}};
            auto staged = direct->StageRestore(descriptor.participant.schemaVersion, descriptor.record, malformed);
            REQUIRE(staged.HasValue());
            CHECK(std::move(staged).Value()->Decode({}).ErrorValue().code.Value() == SaveErrors::RestoreParticipantInvalid.code.Value());
        }

        TEST_CASE("Gameplay restore rejects old module versions and pins its generation until receipt retirement",
                  "[unit][runtime][save][gameplay][reload]") {
            auto source = std::make_shared<StateSource>();
            auto lease = std::make_shared<int>(1);
            std::weak_ptr<int> weakLease = lease;
            const auto descriptor = Descriptor(GameplayPersistenceOwner::Service);
            auto adapter = GameplayPersistenceAdapter::Create(descriptor, source, lease).Value();
            CanonicalStateParticipantRegistry registry;
            REQUIRE(registry.Register(descriptor.participant, adapter).HasValue());
            const auto participants = registry.Snapshot().Value();
            auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants), participants).Value();
            REQUIRE(builder.CaptureParticipants().HasValue());
            auto snapshot = builder.Seal().Value();
            std::vector<std::byte> encoded(snapshot.Records().front().Segment(0).begin(), snapshot.Records().front().Segment(0).end());

            encoded[5] = std::byte{0x06};
            auto outdated = adapter->StageRestore(descriptor.participant.schemaVersion, descriptor.record, encoded);
            REQUIRE(outdated.HasValue());
            auto oldReceipt = std::move(outdated).Value();
            REQUIRE(oldReceipt->Decode({}).HasValue());
            CHECK(oldReceipt->Validate({}).ErrorValue().code.Value() == SaveErrors::RestoreParticipantInvalid.code.Value());
            oldReceipt.reset();

            encoded[5] = std::byte{0x07};
            auto staged = adapter->StageRestore(descriptor.participant.schemaVersion, descriptor.record, encoded);
            REQUIRE(staged.HasValue());
            auto receipt = std::move(staged).Value();
            REQUIRE(registry.Unregister(descriptor.participant.participant).HasValue());
            snapshot = {};
            adapter.reset();
            lease.reset();
            CHECK_FALSE(weakLease.expired());
            receipt.reset();
            CHECK_FALSE(weakLease.expired());  // The pinned builder and registry snapshot still own the capture adapter.
        }

        TEST_CASE("Gameplay state publishes only through the aggregate restore commit", "[unit][runtime][save][gameplay]") {
            auto source = std::make_shared<StateSource>();
            const auto descriptor = Descriptor(GameplayPersistenceOwner::BehaviorInstance);
            auto adapter = GameplayPersistenceAdapter::Create(descriptor, source, std::make_shared<int>(1)).Value();
            CanonicalStateParticipantRegistry registry;
            REQUIRE(registry.Register(descriptor.participant, adapter).HasValue());
            const auto participants = registry.Snapshot().Value();
            auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants), participants).Value();
            REQUIRE(builder.CaptureParticipants().HasValue());
            const auto snapshot = builder.Seal().Value();
            auto staged =
                adapter->StageRestore(descriptor.participant.schemaVersion, descriptor.record, snapshot.Records().front().Segment(0));
            REQUIRE(staged.HasValue());

            source->active = {std::byte{0x55}};
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> receipts;
            receipts.push_back(std::move(staged).Value());
            auto operation = CreateSaveOperation({.operation = 1436, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4});
            REQUIRE(operation.HasValue());
            const StagedRestoreContext context{.operation = 1436,
                                               .registryGeneration = participants.Generation(),
                                               .sessionGeneration = 7,
                                               .sceneIncarnation = 8,
                                               .maximumParticipants = 1};
            auto created = StagedRestoreTransaction::Create(context, std::move(operation).Value(), participants, std::move(receipts));
            REQUIRE(created.HasValue());
            auto transaction = std::move(created).Value();
            REQUIRE(transaction.Prepare().HasValue());
            CHECK(source->active == std::vector<std::byte>{std::byte{0x55}});
            REQUIRE(transaction.Activate({.registryGeneration = participants.Generation(), .sessionGeneration = 7, .sceneIncarnation = 8})
                        .HasValue());
            CHECK(source->active == std::vector<std::byte>{std::byte{0x31}});
        }

    }  // namespace
}  // namespace Horo::Runtime
