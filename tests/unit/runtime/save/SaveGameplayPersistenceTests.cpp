#include "Horo/Gameplay/SaveGameplayPersistence.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveParticipation.h"
#include "SaveTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <exception>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    namespace {
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

        class NoDependencies final : public ICanonicalRestoreDependencyLookup {
        public:
            const ICanonicalRestorePreparedState *Find(const SaveParticipantId &) const noexcept override {
                return nullptr;
            }
        };

        class NoOperations final : public ISaveParticipationOperationHost {
        public:
            Result<SaveOperationHandle> RequestSave(const SaveParticipationSaveRequest &) override {
                return Result<SaveOperationHandle>::Failure(MakeError(SaveErrors::LifecycleUnavailable));
            }

            Result<SaveOperationHandle> RequestLoad(const SaveParticipationLoadRequest &) override {
                return Result<SaveOperationHandle>::Failure(MakeError(SaveErrors::LifecycleUnavailable));
            }
        };

        struct LifetimeFlags final {
            bool unloaded{};
            bool sourceDestroyed{};
            bool candidateDestroyed{};
            bool orderViolation{};
        };

        class ModuleLease final {
        public:
            explicit ModuleLease(std::shared_ptr<LifetimeFlags> flags) : flags_(std::move(flags)) {}

            ModuleLease(const ModuleLease &) = delete;
            ModuleLease &operator=(const ModuleLease &) = delete;
            ModuleLease(ModuleLease &&) = delete;
            ModuleLease &operator=(ModuleLease &&) = delete;

            ~ModuleLease() {
                flags_->unloaded = true;
            }

        private:
            std::shared_ptr<LifetimeFlags> flags_;
        };

        class LifetimeCandidate final : public IPreparedGameplayPersistenceState {
        public:
            explicit LifetimeCandidate(std::shared_ptr<LifetimeFlags> flags) : flags_(std::move(flags)) {}

            LifetimeCandidate(const LifetimeCandidate &) = delete;
            LifetimeCandidate &operator=(const LifetimeCandidate &) = delete;
            LifetimeCandidate(LifetimeCandidate &&) = delete;
            LifetimeCandidate &operator=(LifetimeCandidate &&) = delete;

            ~LifetimeCandidate() override {
                flags_->candidateDestroyed = true;
                flags_->orderViolation |= flags_->unloaded;
            }

            void Publish() noexcept override {
                // This sentinel observes candidate destruction, not publication state.
            }

        private:
            std::shared_ptr<LifetimeFlags> flags_;
        };

        class LifetimeSource final : public IGameplayPersistenceSource {
        public:
            explicit LifetimeSource(std::shared_ptr<LifetimeFlags> flags) : flags_(std::move(flags)) {}

            LifetimeSource(const LifetimeSource &) = delete;
            LifetimeSource &operator=(const LifetimeSource &) = delete;
            LifetimeSource(LifetimeSource &&) = delete;
            LifetimeSource &operator=(LifetimeSource &&) = delete;

            ~LifetimeSource() override {
                flags_->sourceDestroyed = true;
                flags_->orderViolation |= flags_->unloaded;
            }

            Result<std::vector<std::byte>> CaptureRuntimeState(std::uint64_t) const override {
                return Result<std::vector<std::byte>>::Success({std::byte{0x31}});
            }

            Result<std::unique_ptr<IPreparedGameplayPersistenceState>> PrepareRuntimeState(std::span<const std::byte>) override {
                return Result<std::unique_ptr<IPreparedGameplayPersistenceState>>::Success(std::make_unique<LifetimeCandidate>(flags_));
            }

        private:
            std::shared_ptr<LifetimeFlags> flags_;
        };

        static_assert(!std::is_copy_constructible_v<ModuleLease> && !std::is_copy_assignable_v<ModuleLease> &&
                      !std::is_move_constructible_v<ModuleLease> && !std::is_move_assignable_v<ModuleLease>);
        static_assert(!std::is_copy_constructible_v<LifetimeCandidate> && !std::is_copy_assignable_v<LifetimeCandidate> &&
                      !std::is_move_constructible_v<LifetimeCandidate> && !std::is_move_assignable_v<LifetimeCandidate>);
        static_assert(!std::is_copy_constructible_v<LifetimeSource> && !std::is_copy_assignable_v<LifetimeSource> &&
                      !std::is_move_constructible_v<LifetimeSource> && !std::is_move_assignable_v<LifetimeSource>);

        [[nodiscard]] GameplayPersistenceDescriptor Descriptor(const GameplayPersistenceOwner owner, const std::uint8_t identity = 1) {
            GameplayPersistenceDescriptor descriptor;
            descriptor.participant.participant = SaveParticipantId::Parse("project.gameplay.state").Value();
            descriptor.participant.schemaVersion = Test::V<ParticipantSchemaVersion>(3);
            descriptor.participant.scope =
                owner == GameplayPersistenceOwner::Session ? SaveParticipantScope::SlotPlayer : SaveParticipantScope::RuntimeScene;
            descriptor.participant.roles = SaveParticipantRole::Capture | SaveParticipantRole::Restore;
            descriptor.participant.limits = {.maximumPayloadBytes = 128, .maximumRecordCount = 1, .maximumNestingDepth = 4};
            descriptor.record = Test::Id<SaveRecordId>(identity);
            descriptor.participant.ownedRecords = {descriptor.record};
            descriptor.owner = owner;
            descriptor.moduleId = SaveParticipantId::Parse("project.gameplay.module").Value();
            descriptor.moduleVersion = 7;
            return descriptor;
        }

        [[nodiscard]] RuntimeSaveCaptureProvenance Provenance(const SaveParticipantRegistrySnapshot &participants) {
            return {.capturedState = Test::Id<CapturedStateId>(9),
                    .epoch = CanonicalCaptureEpoch{11},
                    .sceneIncarnation = 4,
                    .sceneRevision = 5,
                    .registryGeneration = participants.Generation()};
        }

        /** @brief Captures explicit registered state with no ambient save host. */
        [[nodiscard]] RuntimeSaveSnapshot CaptureRegisteredState(const std::shared_ptr<GameplayPersistenceAdapter> &adapter) {
            CanonicalStateParticipantRegistry registry;
            REQUIRE(registry.Register(adapter->Descriptor().participant, adapter).HasValue());
            const auto participants = registry.Snapshot().Value();
            auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants), participants).Value();
            REQUIRE(builder.CaptureParticipants().HasValue());
            return builder.Seal().Value();
        }

        /** @brief Leaves a receipt as the sole owner of project source and generation lease. */
        [[nodiscard]] std::unique_ptr<IStagedRestoreParticipant> DetachedLifetimeReceipt(const std::shared_ptr<LifetimeFlags> &flags,
                                                                                         const GameplayPersistenceOwner owner) {
            const auto descriptor = Descriptor(owner);
            auto lease = std::make_shared<ModuleLease>(flags);
            auto source = std::make_shared<LifetimeSource>(flags);
            auto adapter = GameplayPersistenceAdapter::Create(descriptor, source, lease).Value();
            const auto snapshot = CaptureRegisteredState(adapter);
            auto staged =
                adapter->StageRestore(descriptor.participant.schemaVersion, descriptor.record, snapshot.Records().front().Segment(0));
            REQUIRE(staged.HasValue());
            return std::move(staged).Value();
        }

        TEST_CASE("Rejected gameplay adapter creation destroys its source before releasing the incoming lease",
                  "[unit][runtime][save][gameplay][lifetime]") {
            auto flags = std::make_shared<LifetimeFlags>();
            auto descriptor = Descriptor(GameplayPersistenceOwner::Service);
            descriptor.moduleVersion = 0;
            auto lease = std::make_shared<ModuleLease>(flags);
            auto source = std::make_shared<LifetimeSource>(flags);
            CHECK(GameplayPersistenceAdapter::Create(descriptor, std::move(source), std::move(lease)).HasError());
            CHECK(flags->sourceDestroyed);
            CHECK(flags->unloaded);
            CHECK_FALSE(flags->orderViolation);
        }

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

        TEST_CASE("Capture snapshots release module code only after source destruction", "[unit][runtime][save][gameplay][lifetime]") {
            auto flags = std::make_shared<LifetimeFlags>();
            RuntimeSaveSnapshot archived;
            {
                const auto descriptor = Descriptor(GameplayPersistenceOwner::Service);
                auto lease = std::make_shared<ModuleLease>(flags);
                auto source = std::make_shared<LifetimeSource>(flags);
                auto adapter = GameplayPersistenceAdapter::Create(descriptor, source, lease).Value();
                CanonicalStateParticipantRegistry registry;
                NoOperations operations;
                SaveParticipationCapabilities capabilities;
                capabilities.captureParticipants = true;
                capabilities.restoreParticipants = true;
                auto createdHost = SaveParticipationHost::Create(1, capabilities, registry, operations);
                REQUIRE(createdHost.HasValue());
                auto host = std::move(createdHost).Value();
                REQUIRE(host.Client().RegisterParticipant(descriptor.participant, adapter).HasValue());
                auto participants = registry.Snapshot().Value();
                auto builder = RuntimeSaveCaptureBuilder::Create(Provenance(participants), participants).Value();
                REQUIRE(builder.CaptureParticipants().HasValue());
                auto snapshot = builder.Seal().Value();
                archived = snapshot;
                REQUIRE(host.Close().HasValue());
                adapter.reset();
                source.reset();
                lease.reset();
                CHECK_FALSE(flags->unloaded);
                CHECK_FALSE(flags->sourceDestroyed);
                CHECK(snapshot.Records().size() == 1);
            }
            // Detached archive work outlives the host, registry, builder and caller-owned handles.
            CHECK_FALSE(flags->sourceDestroyed);
            CHECK_FALSE(flags->unloaded);
            REQUIRE(archived.Records().size() == 1);
            CHECK(archived.Records().front().Segment(0).back() == std::byte{0x31});
            archived = {};
            CHECK(flags->sourceDestroyed);
            CHECK(flags->unloaded);
            CHECK_FALSE(flags->orderViolation);
        }

        TEST_CASE("Rollback destroys prepared gameplay state and source before module unload",
                  "[unit][runtime][save][gameplay][lifetime]") {
            auto flags = std::make_shared<LifetimeFlags>();
            auto receipt = DetachedLifetimeReceipt(flags, GameplayPersistenceOwner::ModuleGlobal);
            CHECK_FALSE(flags->unloaded);
            REQUIRE(receipt->Decode({}).HasValue());
            REQUIRE(receipt->Validate({}).HasValue());
            REQUIRE(receipt->Instantiate({}).HasValue());
            const NoDependencies dependencies;
            REQUIRE(receipt->ApplyState(dependencies).HasValue());
            receipt->RollbackPrepared();
            CHECK(flags->candidateDestroyed);
            CHECK_FALSE(flags->unloaded);
            CHECK_FALSE(flags->orderViolation);
            receipt.reset();
            CHECK(flags->sourceDestroyed);
            CHECK(flags->unloaded);
            CHECK_FALSE(flags->orderViolation);
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

        TEST_CASE("Published gameplay candidate retires before its module lease", "[unit][runtime][save][gameplay][lifetime]") {
            auto flags = std::make_shared<LifetimeFlags>();
            auto receipt = DetachedLifetimeReceipt(flags, GameplayPersistenceOwner::Session);
            REQUIRE(receipt->Decode({}).HasValue());
            REQUIRE(receipt->Validate({}).HasValue());
            REQUIRE(receipt->Instantiate({}).HasValue());
            const NoDependencies dependencies;
            REQUIRE(receipt->ApplyState(dependencies).HasValue());
            REQUIRE(receipt->FixupReferences(dependencies).HasValue());
            receipt->PublishPrepared();
            receipt.reset();
            CHECK(flags->candidateDestroyed);
            CHECK(flags->sourceDestroyed);
            CHECK(flags->unloaded);
            CHECK_FALSE(flags->orderViolation);
        }
    }  // namespace
}  // namespace Horo::Runtime
