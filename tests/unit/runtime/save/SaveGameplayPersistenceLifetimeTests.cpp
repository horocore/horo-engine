#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveParticipation.h"
#include "SaveGameplayPersistenceTestUtils.h"

#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    namespace {
        using namespace Test::GameplayPersistence;

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
