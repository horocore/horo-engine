#include "AllocationProbe.h"
#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveRestoreTransaction.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    namespace {
        SaveParticipantId Participant(const std::string_view value) {
            return SaveParticipantId::Parse(value).Value();
        }

        SaveRecordId Record(const std::uint8_t suffix) {
            SaveIdentityDetail::Bytes bytes{};
            bytes.back() = suffix;
            return SaveRecordId::FromBytes(bytes).Value();
        }

        ParticipantSchemaVersion Schema(const std::uint32_t value = 1) {
            return ParticipantSchemaVersion::Create(value).Value();
        }

        class RegistryAdapter final : public ICanonicalStateAdapter {
        public:
            Result<CanonicalCaptureDisposition> Capture(const CanonicalCaptureContext &, ICanonicalCaptureSink &) const override {
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Omitted);
            }
        };

        CanonicalStateParticipantDescriptor Descriptor(const std::string_view participant, const std::uint8_t record,
                                                       const bool required = true,
                                                       std::vector<SaveParticipantDependency> dependencies = {}) {
            return {.participant = Participant(participant),
                    .schemaVersion = Schema(),
                    .scope = SaveParticipantScope::RuntimeScene,
                    .roles = SaveParticipantRole::Restore,
                    .required = required,
                    .limits = {.maximumPayloadBytes = 1024, .maximumRecordCount = 1, .maximumNestingDepth = 4},
                    .dependencies = std::move(dependencies),
                    .ownedRecords = {Record(record)}};
        }

        SaveParticipantRegistrySnapshot Registry(std::vector<CanonicalStateParticipantDescriptor> descriptors) {
            CanonicalStateParticipantRegistry registry;
            for (const auto &descriptor : descriptors)
                REQUIRE(registry.Register(descriptor, std::make_shared<RegistryAdapter>()).HasValue());
            return registry.Snapshot().Value();
        }

        SaveOperationController Operation(const OperationId id = 1392) {
            return CreateSaveOperation({.operation = id, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4}).Value();
        }

        StagedRestoreContext Context(const SaveParticipantRegistrySnapshot &registry, const OperationId operation = 1392,
                                     const std::size_t maximumParticipants = 8) {
            return {.operation = operation,
                    .registryGeneration = registry.Generation(),
                    .sessionGeneration = 17,
                    .sceneIncarnation = 23,
                    .maximumParticipants = maximumParticipants};
        }

        StagedRestoreActivationEvidence Activation(const SaveParticipantRegistrySnapshot &registry,
                                                   const std::uint64_t sessionGeneration = 17, const std::uint64_t sceneIncarnation = 23) {
            return {.registryGeneration = registry.Generation(),
                    .sessionGeneration = sessionGeneration,
                    .sceneIncarnation = sceneIncarnation};
        }

        enum class InjectedFailure : std::uint8_t {
            None,
            Decode,
            Validate,
            Instantiate,
            Apply,
            Fixup,
            Throw,
        };

        struct ParticipantLog final {
            std::vector<std::string> phases;
            std::vector<std::string> published;
            std::vector<std::string> rolledBack;
            std::vector<std::string> dependencies;
        };

        class PreparedState final : public ICanonicalRestorePreparedState {};

        class StagedParticipant final : public IStagedRestoreParticipant {
        public:
            StagedParticipant(std::string_view participant, ParticipantLog &log, const InjectedFailure failure = InjectedFailure::None,
                              const std::uint32_t schema = 1)
                : requirement_{.participant = Participant(participant),
                               .schemaVersion = Schema(schema),
                               .scope = SaveParticipantScope::RuntimeScene},
                  log_(log), failure_(failure) {}

            const StagedRestoreParticipantRequirement &Requirement() const noexcept override {
                return requirement_;
            }

            Result<void> Decode(const StagedRestoreContext &) override {
                return Run("decode", InjectedFailure::Decode);
            }

            Result<void> Validate(const StagedRestoreContext &) override {
                return Run("validate", InjectedFailure::Validate);
            }

            Result<void> Instantiate(const StagedRestoreContext &) override {
                auto result = Run("instantiate", InjectedFailure::Instantiate);
                if (result.HasValue())
                    prepared_ = true;
                return result;
            }

            const ICanonicalRestorePreparedState *PreparedState() const noexcept override {
                return prepared_ ? &state_ : nullptr;
            }

            Result<void> ApplyState(const ICanonicalRestoreDependencyLookup &dependencies) override {
                ObserveDependencies(dependencies);
                return Run("apply", InjectedFailure::Apply);
            }

            Result<void> FixupReferences(const ICanonicalRestoreDependencyLookup &dependencies) override {
                ObserveDependencies(dependencies);
                return Run("fixup", InjectedFailure::Fixup);
            }

            void PublishPrepared() noexcept override {
                Finalize(true);
            }

            void RollbackPrepared() noexcept override {
                Finalize(false);
            }

            void Observe(std::vector<SaveParticipantId> dependencies) {
                observedDependencies_ = std::move(dependencies);
            }

        private:
            void Finalize(const bool publish) noexcept {
                if (publish) {
                    log_.published.push_back(requirement_.participant.Value());
                    published_ = true;
                } else if (!published_ && !rolledBack_) {
                    log_.rolledBack.push_back(requirement_.participant.Value());
                    rolledBack_ = true;
                }
            }

            Result<void> Run(const std::string_view phase, const InjectedFailure phaseFailure) {
                log_.phases.push_back(requirement_.participant.Value() + ":" + std::string{phase});
                if (failure_ == InjectedFailure::Throw && phase == "apply")
                    throw std::runtime_error{"participant contract violation"};
                if (failure_ == phaseFailure)
                    return Result<void>::Failure(MakeError(SaveErrors::CompositionInjectedFailure));
                return Result<void>::Success();
            }

            void ObserveDependencies(const ICanonicalRestoreDependencyLookup &dependencies) {
                for (const SaveParticipantId &dependency : observedDependencies_) {
                    const std::string result = dependencies.Find(dependency) == nullptr ? "missing" : "visible";
                    log_.dependencies.push_back(requirement_.participant.Value() + ":" + dependency.Value() + ":" + result);
                }
            }

            StagedRestoreParticipantRequirement requirement_;
            ParticipantLog &log_;
            InjectedFailure failure_{InjectedFailure::None};
            ::Horo::Runtime::PreparedState state_;
            std::vector<SaveParticipantId> observedDependencies_;
            bool prepared_{};
            bool published_{};
            bool rolledBack_{};
        };

        std::unique_ptr<StagedParticipant> Candidate(std::string_view participant, ParticipantLog &log,
                                                     const InjectedFailure failure = InjectedFailure::None) {
            return std::make_unique<StagedParticipant>(participant, log, failure);
        }

        SaveOperationSnapshot Snapshot(const SaveOperationHandle &handle) {
            const auto snapshot = handle.Snapshot();
            REQUIRE(snapshot.has_value());
            return *snapshot;
        }

        void RequireError(const Result<void> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        std::vector<StagedRestorePhase> TracePhases(const StagedRestoreTransaction &transaction) {
            std::vector<StagedRestorePhase> phases;
            for (const auto &event : transaction.Trace())
                phases.push_back(event.phase);
            return phases;
        }

        SaveParticipantRegistrySnapshot DependencyRegistry() {
            const SaveParticipantId providerId = Participant("horo.test.provider");
            return Registry(
                {Descriptor("horo.test.consumer", 2, true,
                            {{providerId, SaveParticipantDependencyRequirement::Required, SaveParticipantDependencyPhase::Restore}}),
                 Descriptor("horo.test.provider", 1)});
        }

        TEST_CASE("Staged restore prepares dependencies deterministically and publishes one complete bundle",
                  "[unit][save][restore][activation]") {
            const SaveParticipantId providerId = Participant("horo.test.provider");
            const auto registry = DependencyRegistry();
            ParticipantLog log;
            auto consumer = Candidate("horo.test.consumer", log);
            consumer->Observe({providerId, Participant("horo.test.undeclared")});
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> staged;
            staged.push_back(std::move(consumer));
            staged.push_back(Candidate("horo.test.provider", log));
            auto operation = Operation();
            const SaveOperationHandle handle = operation.Handle();
            auto transaction =
                StagedRestoreTransaction::Create(Context(registry), std::move(operation), registry, std::move(staged)).Value();

            REQUIRE(transaction.Prepare().HasValue());
            CHECK(transaction.State() == StagedRestoreTransactionState::ReadyToActivate);
            REQUIRE(transaction.Activate(Activation(registry)).HasValue());
            CHECK(transaction.State() == StagedRestoreTransactionState::Activated);
            CHECK(log.published == std::vector<std::string>{"horo.test.provider", "horo.test.consumer"});
            CHECK(log.rolledBack.empty());
            CHECK(log.dependencies == std::vector<std::string>{"horo.test.consumer:horo.test.provider:visible",
                                                               "horo.test.consumer:horo.test.undeclared:missing",
                                                               "horo.test.consumer:horo.test.provider:visible",
                                                               "horo.test.consumer:horo.test.undeclared:missing"});
            const auto terminal = Snapshot(handle);
            CHECK(terminal.state == SaveOperationState::Completed);
            CHECK(terminal.commit == SaveOperationCommitOutcome::Committed);

            const auto phases = TracePhases(transaction);
            REQUIRE(phases.size() == 14);
            CHECK(phases[0] == StagedRestorePhase::Decode);
            CHECK(phases[2] == StagedRestorePhase::Validate);
            CHECK(phases[4] == StagedRestorePhase::Plan);
            CHECK(phases[5] == StagedRestorePhase::Instantiate);
            CHECK(phases[7] == StagedRestorePhase::ApplyState);
            CHECK(phases[9] == StagedRestorePhase::FixupReferences);
            CHECK(phases[11] == StagedRestorePhase::ReadyToActivate);
            CHECK(phases[12] == StagedRestorePhase::Activate);
            for (std::size_t index{}; index < transaction.Trace().size(); ++index)
                CHECK(transaction.Trace()[index].sequence == index + 1U);
        }

        TEST_CASE("Every fallible restore phase rolls inactive candidates back in reverse dependency order",
                  "[unit][save][restore][rollback]") {
            const auto registry = DependencyRegistry();
            for (const InjectedFailure failure : {InjectedFailure::Decode, InjectedFailure::Validate, InjectedFailure::Instantiate,
                                                  InjectedFailure::Apply, InjectedFailure::Fixup, InjectedFailure::Throw}) {
                ParticipantLog log;
                std::vector<std::unique_ptr<IStagedRestoreParticipant>> staged;
                staged.push_back(Candidate("horo.test.provider", log));
                staged.push_back(Candidate("horo.test.consumer", log, failure));
                auto operation = Operation(200 + static_cast<OperationId>(failure));
                const SaveOperationHandle handle = operation.Handle();
                auto transaction =
                    StagedRestoreTransaction::Create(Context(registry, handle.Id()), std::move(operation), registry, std::move(staged))
                        .Value();

                const auto prepared = transaction.Prepare();
                REQUIRE(prepared.HasError());
                CHECK(transaction.State() == StagedRestoreTransactionState::Failed);
                CHECK(log.published.empty());
                CHECK(log.rolledBack == std::vector<std::string>{"horo.test.consumer", "horo.test.provider"});
                const auto terminal = Snapshot(handle);
                CHECK(terminal.state == SaveOperationState::Failed);
                CHECK(terminal.commit == SaveOperationCommitOutcome::NotCommitted);
                REQUIRE(terminal.terminalError.has_value());
                const auto &expected =
                    failure == InjectedFailure::Throw ? SaveErrors::RestoreAdapterContractInvalid : SaveErrors::CompositionInjectedFailure;
                CHECK(terminal.terminalError->code.Value() == expected.code.Value());
                CHECK(transaction.Trace().back().phase == StagedRestorePhase::Rollback);
            }
        }

        TEST_CASE("Cancellation and stale activation leave the active world unpublished", "[unit][save][restore][lifecycle]") {
            const auto registry = Registry({Descriptor("horo.test.scene", 1)});

            ParticipantLog cancellationLog;
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> cancelledStaged;
            cancelledStaged.push_back(Candidate("horo.test.scene", cancellationLog));
            auto cancelledOperation = Operation(301);
            const auto cancelledHandle = cancelledOperation.Handle();
            auto cancelled = StagedRestoreTransaction::Create(Context(registry, 301), std::move(cancelledOperation), registry,
                                                              std::move(cancelledStaged))
                                 .Value();
            REQUIRE(cancelled.Prepare().HasValue());
            REQUIRE(cancelledHandle.RequestCancellation() == SaveCancellationRequestResult::Requested);
            RequireError(cancelled.Activate(Activation(registry)), SaveErrors::OperationCancelled);
            CHECK(cancelled.State() == StagedRestoreTransactionState::RolledBack);
            CHECK(cancellationLog.published.empty());
            CHECK(cancellationLog.rolledBack == std::vector<std::string>{"horo.test.scene"});
            CHECK(Snapshot(cancelledHandle).state == SaveOperationState::Cancelled);

            std::vector<StagedRestoreActivationEvidence> staleEvidence(3, Activation(registry));
            ++staleEvidence[0].registryGeneration;
            ++staleEvidence[1].sessionGeneration;
            ++staleEvidence[2].sceneIncarnation;
            for (std::size_t index{}; index < staleEvidence.size(); ++index) {
                ParticipantLog staleLog;
                std::vector<std::unique_ptr<IStagedRestoreParticipant>> staleStaged;
                staleStaged.push_back(Candidate("horo.test.scene", staleLog));
                const OperationId operationId = 302 + index;
                auto staleOperation = Operation(operationId);
                const auto staleHandle = staleOperation.Handle();
                auto stale = StagedRestoreTransaction::Create(Context(registry, operationId), std::move(staleOperation), registry,
                                                              std::move(staleStaged))
                                 .Value();
                REQUIRE(stale.Prepare().HasValue());
                RequireError(stale.Activate(staleEvidence[index]), SaveErrors::RestoreActivationStale);
                CHECK(stale.State() == StagedRestoreTransactionState::Failed);
                CHECK(staleLog.published.empty());
                CHECK(staleLog.rolledBack == std::vector<std::string>{"horo.test.scene"});
                CHECK(Snapshot(staleHandle).terminalError->code.Value() == SaveErrors::RestoreActivationStale.code.Value());
            }
        }

        TEST_CASE("Cancellation before restore preparation rolls staging back without participant work",
                  "[unit][save][restore][lifecycle]") {
            const auto registry = Registry({Descriptor("horo.test.scene", 1)});
            ParticipantLog log;
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> staged;
            staged.push_back(Candidate("horo.test.scene", log));
            auto earlyOperation = Operation(303);
            const auto earlyHandle = earlyOperation.Handle();
            auto early =
                StagedRestoreTransaction::Create(Context(registry, 303), std::move(earlyOperation), registry, std::move(staged)).Value();
            REQUIRE(earlyHandle.RequestCancellation() == SaveCancellationRequestResult::Requested);
            RequireError(early.Prepare(), SaveErrors::OperationCancelled);
            CHECK(early.State() == StagedRestoreTransactionState::RolledBack);
            CHECK(log.phases.empty());
            CHECK(log.rolledBack == std::vector<std::string>{"horo.test.scene"});
        }

        TEST_CASE("Restore creation rejects incomplete duplicate mismatched and over-capacity input transactionally",
                  "[unit][save][restore][validation]") {
            const auto registry = Registry({Descriptor("horo.test.required", 1), Descriptor("horo.test.optional", 2, false)});

            ParticipantLog missingLog;
            auto missingOperation = Operation(401);
            const auto missingHandle = missingOperation.Handle();
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> missing;
            auto missingResult =
                StagedRestoreTransaction::Create(Context(registry, 401), std::move(missingOperation), registry, std::move(missing));
            REQUIRE(missingResult.HasError());
            CHECK(missingResult.ErrorValue().code.Value() == SaveErrors::RestoreParticipantIncomplete.code.Value());
            CHECK(Snapshot(missingHandle).terminalError->code.Value() == SaveErrors::RestoreParticipantIncomplete.code.Value());

            ParticipantLog duplicateLog;
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> duplicate;
            duplicate.push_back(Candidate("horo.test.required", duplicateLog));
            duplicate.push_back(Candidate("horo.test.required", duplicateLog));
            auto duplicateOperation = Operation(402);
            auto duplicateResult =
                StagedRestoreTransaction::Create(Context(registry, 402), std::move(duplicateOperation), registry, std::move(duplicate));
            REQUIRE(duplicateResult.HasError());
            CHECK(duplicateResult.ErrorValue().code.Value() == SaveErrors::RestoreParticipantInvalid.code.Value());
            CHECK(duplicateLog.rolledBack.size() == 2);

            ParticipantLog mismatchLog;
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> mismatch;
            mismatch.push_back(std::make_unique<StagedParticipant>("horo.test.required", mismatchLog, InjectedFailure::None, 2));
            auto mismatchOperation = Operation(403);
            auto mismatchResult =
                StagedRestoreTransaction::Create(Context(registry, 403), std::move(mismatchOperation), registry, std::move(mismatch));
            REQUIRE(mismatchResult.HasError());
            CHECK(mismatchResult.ErrorValue().code.Value() == SaveErrors::RestoreParticipantInvalid.code.Value());
            CHECK(mismatchLog.rolledBack == std::vector<std::string>{"horo.test.required"});

            ParticipantLog capacityLog;
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> overCapacity;
            overCapacity.push_back(Candidate("horo.test.required", capacityLog));
            auto capacityOperation = Operation(404);
            auto capacityResult = StagedRestoreTransaction::Create(Context(registry, 404, 0), std::move(capacityOperation), registry,
                                                                   std::move(overCapacity));
            REQUIRE(capacityResult.HasError());
            CHECK(capacityResult.ErrorValue().code.Value() == SaveErrors::RestoreContextInvalid.code.Value());
            CHECK(capacityLog.rolledBack == std::vector<std::string>{"horo.test.required"});
        }

        TEST_CASE("Restore creation permits an omitted optional participant", "[unit][save][restore][validation]") {
            const auto registry = Registry({Descriptor("horo.test.required", 1), Descriptor("horo.test.optional", 2, false)});
            ParticipantLog log;
            std::vector<std::unique_ptr<IStagedRestoreParticipant>> staged;
            staged.push_back(Candidate("horo.test.required", log));
            auto operation = Operation(405);
            auto restore =
                StagedRestoreTransaction::Create(Context(registry, 405), std::move(operation), registry, std::move(staged)).Value();
            REQUIRE(restore.Prepare().HasValue());
            REQUIRE(restore.Activate(Activation(registry)).HasValue());
            CHECK(log.published == std::vector<std::string>{"horo.test.required"});
        }

        TEST_CASE("Dropping a ready restore rolls each candidate back exactly once", "[unit][save][restore][ownership]") {
            const auto registry = Registry({Descriptor("horo.test.scene", 1)});
            ParticipantLog log;
            SaveOperationHandle handle;
            {
                std::vector<std::unique_ptr<IStagedRestoreParticipant>> staged;
                staged.push_back(Candidate("horo.test.scene", log));
                auto operation = Operation(501);
                handle = operation.Handle();
                auto transaction =
                    StagedRestoreTransaction::Create(Context(registry, 501), std::move(operation), registry, std::move(staged)).Value();
                REQUIRE(transaction.Prepare().HasValue());
                auto owner = std::move(transaction);
                CHECK(transaction.State() == StagedRestoreTransactionState::RolledBack);
                CHECK(owner.State() == StagedRestoreTransactionState::ReadyToActivate);
            }
            CHECK(log.published.empty());
            CHECK(log.rolledBack == std::vector<std::string>{"horo.test.scene"});
            CHECK(Snapshot(handle).terminalError->code.Value() == SaveErrors::OperationAbandoned.code.Value());
        }

        TEST_CASE("Restore admission maps every bookkeeping allocation failure to a typed terminal result",
                  "[unit][save][restore][allocation]") {
            const auto registry = Registry({Descriptor("horo.test.scene", 1)});
            bool admitted = false;
            for (std::size_t successfulAllocations{}; successfulAllocations < 32 && !admitted; ++successfulAllocations) {
                ParticipantLog log;
                std::vector<std::unique_ptr<IStagedRestoreParticipant>> staged;
                staged.push_back(Candidate("horo.test.scene", log));
                auto operation = Operation(600 + successfulAllocations);
                const auto handle = operation.Handle();
                auto created = [&] {
                    Tests::AllocationProbe::ScopedFailure failure{successfulAllocations};
                    return StagedRestoreTransaction::Create(Context(registry, handle.Id()), std::move(operation), registry,
                                                            std::move(staged));
                }();
                admitted = created.HasValue();
                if (!admitted) {
                    CHECK(created.ErrorValue().code.Value() == SaveErrors::RestoreAllocationFailed.code.Value());
                    CHECK(Snapshot(handle).terminalError->code.Value() == SaveErrors::RestoreAllocationFailed.code.Value());
                    CHECK(log.rolledBack == std::vector<std::string>{"horo.test.scene"});
                }
            }
            CHECK(admitted);
        }
    }  // namespace
}  // namespace Horo::Runtime
