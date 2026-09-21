#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveSlotRecovery.h"
#include "SaveTestUtils.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    namespace {
        using namespace Test;

        [[nodiscard]] ImmutableSaveArchive Archive(const std::uint8_t value) {
            return {.bytes = std::make_shared<const std::vector<std::byte>>(
                        std::initializer_list<std::byte>{std::byte{0x48}, static_cast<std::byte>(value)})};
        }

        [[nodiscard]] SaveSlotRecoveryArtifact Artifact(const std::uint8_t generation, const std::uint64_t sequence,
                                                        const std::uint8_t slot = 4) {
            return {.metadata = Entry(slot, generation), .archive = Archive(generation), .retentionSequence = sequence};
        }

        class FakeValidator final : public ISaveSlotRecoveryValidator {
        public:
            std::vector<std::pair<std::uint64_t, SaveSlotRecoveryValidationState>> states;
            std::optional<Error> failure;
            bool returnUnknownState{};

            [[nodiscard]] Result<SaveSlotRecoveryValidation> Validate(const SaveSlotRecoveryArtifact &artifact,
                                                                      const SaveGameSlotId expectedSlot) const override {
                if (failure)
                    return Result<SaveSlotRecoveryValidation>::Failure(*failure);
                if (!artifact.metadata || artifact.metadata->publication.slot != expectedSlot)
                    return Result<SaveSlotRecoveryValidation>::Success(
                        {.state = SaveSlotRecoveryValidationState::Incompatible, .diagnostic = MakeError(SaveErrors::SlotRecoveryInvalid)});
                if (returnUnknownState)
                    return Result<SaveSlotRecoveryValidation>::Success({.state = SaveSlotRecoveryValidationState::Count});
                SaveSlotRecoveryValidation validation{.state = SaveSlotRecoveryValidationState::Valid};
                for (const auto &[sequence, state] : states) {
                    if (sequence == artifact.retentionSequence) {
                        validation.state = state;
                        break;
                    }
                }
                return Result<SaveSlotRecoveryValidation>::Success(std::move(validation));
            }
        };

        [[nodiscard]] SaveSlotRecoveryPlan Build(FakeValidator &validator, const SaveSlotRecoveryRequest &request,
                                                 const SaveSlotRecoveryPolicy policy = {}) {
            SaveSlotRecoveryPlanner planner(validator, policy);
            auto result = planner.Build(request);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        [[nodiscard]] std::vector<std::uint64_t> Sequences(const std::vector<SaveSlotRecoveryArtifact> &artifacts) {
            std::vector<std::uint64_t> result;
            result.reserve(artifacts.size());
            for (const auto &artifact : artifacts)
                result.push_back(artifact.retentionSequence);
            return result;
        }

        TEST_CASE("Recovery validates candidates, separates corrupt evidence, and exposes only valid promotion input",
                  "[unit][runtime][save][slot-recovery]") {
            FakeValidator validator;
            validator.states = {{1, SaveSlotRecoveryValidationState::Corrupt},
                                {2, SaveSlotRecoveryValidationState::Incompatible},
                                {3, SaveSlotRecoveryValidationState::Valid},
                                {100, SaveSlotRecoveryValidationState::Corrupt}};
            const auto current = Artifact(90, 100);
            const std::vector backups{Artifact(1, 1), Artifact(3, 3), Artifact(2, 2)};
            const SaveSlotRecoveryRequest request{.slot = Id<SaveGameSlotId>(4),
                                                  .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                  .current = current,
                                                  .backups = backups,
                                                  .quarantined = {}};

            const auto plan = Build(validator, request);
            REQUIRE(plan.currentValidation);
            CHECK(plan.currentValidation->state == SaveSlotRecoveryValidationState::Corrupt);
            REQUIRE(plan.promotion);
            CHECK(plan.promotion->artifact.retentionSequence == 3);
            CHECK(plan.decision == SaveSlotRecoveryDecision::AutomaticPromotion);
            CHECK(plan.decisionReason == SaveSlotRecoveryDecisionReason::AutomaticPolicy);
            REQUIRE(plan.quarantine.size() == 2);
            CHECK(plan.quarantine[0].validation.state == SaveSlotRecoveryValidationState::Incompatible);
            CHECK(plan.quarantine[1].validation.state == SaveSlotRecoveryValidationState::Corrupt);
            CHECK(plan.quarantine[0].artifact.retentionSequence == 2);
            CHECK(plan.quarantine[1].artifact.retentionSequence == 1);
            const auto retainedQuarantine = Sequences(plan.retainedQuarantine);
            CHECK(std::ranges::find(retainedQuarantine, 100) != retainedQuarantine.end());
        }

        TEST_CASE("Recovery retention is bounded and independent of scanner order", "[unit][runtime][save][slot-recovery]") {
            FakeValidator validator;
            validator.states = {{100, SaveSlotRecoveryValidationState::Corrupt}};
            const auto current = Artifact(90, 100);
            const std::vector first{Artifact(1, 1), Artifact(4, 4), Artifact(2, 2), Artifact(3, 3)};
            const std::vector second{Artifact(3, 3), Artifact(1, 1), Artifact(4, 4), Artifact(2, 2)};
            const SaveSlotRecoveryPolicy policy{.maximumBackups = 2,
                                                .maximumQuarantined = 2,
                                                .maximumObservedArtifacts = 8,
                                                .automaticPromotion = SaveSlotRecoveryAutomaticPolicy::CorruptCurrent};
            const SaveSlotRecoveryRequest firstRequest{.slot = Id<SaveGameSlotId>(4),
                                                       .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                       .current = current,
                                                       .backups = first,
                                                       .quarantined = {}};
            const SaveSlotRecoveryRequest secondRequest{.slot = Id<SaveGameSlotId>(4),
                                                        .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                        .current = current,
                                                        .backups = second,
                                                        .quarantined = {}};

            const auto firstPlan = Build(validator, firstRequest, policy);
            const auto secondPlan = Build(validator, secondRequest, policy);
            CHECK(Sequences(firstPlan.retainedBackups) == std::vector<std::uint64_t>{4, 3});
            CHECK(Sequences(secondPlan.retainedBackups) == std::vector<std::uint64_t>{4, 3});
            REQUIRE(firstPlan.cleanup.size() == 2);
            REQUIRE(secondPlan.cleanup.size() == 2);
            CHECK(firstPlan.cleanup[0].artifact.retentionSequence == 2);
            CHECK(firstPlan.cleanup[1].artifact.retentionSequence == 1);
            CHECK(Sequences(firstPlan.retainedQuarantine) == std::vector<std::uint64_t>{100});
            CHECK(Sequences(secondPlan.retainedQuarantine) == std::vector<std::uint64_t>{100});
        }

        TEST_CASE("A valid current generation suppresses recovery promotion", "[unit][runtime][save][slot-recovery]") {
            FakeValidator validator;
            const auto current = Artifact(90, 100);
            const std::vector backups{Artifact(1, 1)};
            const SaveSlotRecoveryRequest request{.slot = Id<SaveGameSlotId>(4),
                                                  .trigger = SaveSlotRecoveryTrigger::InterruptedPublication,
                                                  .current = current,
                                                  .backups = backups,
                                                  .quarantined = {}};

            const auto plan = Build(validator, request);
            CHECK_FALSE(plan.HasPromotion());
            CHECK(plan.decision == SaveSlotRecoveryDecision::NoRecovery);
            CHECK(plan.decisionReason == SaveSlotRecoveryDecisionReason::CurrentValid);
            CHECK(plan.cleanup.empty());
        }

        TEST_CASE("Recovery does not propose cleanup when no valid promotion exists", "[unit][runtime][save][slot-recovery]") {
            FakeValidator validator;
            validator.states = {{100, SaveSlotRecoveryValidationState::Corrupt},
                                {1, SaveSlotRecoveryValidationState::Corrupt},
                                {2, SaveSlotRecoveryValidationState::Corrupt}};
            const auto current = Artifact(90, 100);
            const std::vector backups{Artifact(1, 1), Artifact(2, 2)};
            const SaveSlotRecoveryRequest request{.slot = Id<SaveGameSlotId>(4),
                                                  .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                  .current = current,
                                                  .backups = backups,
                                                  .quarantined = {}};

            const auto plan = Build(validator, request, {.maximumBackups = 1, .maximumQuarantined = 2, .maximumObservedArtifacts = 8});
            CHECK_FALSE(plan.HasPromotion());
            CHECK(plan.decisionReason == SaveSlotRecoveryDecisionReason::NoValidBackup);
            CHECK(plan.cleanup.empty());
        }

        TEST_CASE("Incompatible current data requires explicit confirmation even with a valid backup",
                  "[unit][runtime][save][slot-recovery]") {
            FakeValidator validator;
            validator.states = {{100, SaveSlotRecoveryValidationState::Incompatible}};
            const auto current = Artifact(90, 100);
            const std::vector backups{Artifact(1, 1)};
            const SaveSlotRecoveryRequest request{.slot = Id<SaveGameSlotId>(4),
                                                  .trigger = SaveSlotRecoveryTrigger::IncompatibleCurrent,
                                                  .current = current,
                                                  .backups = backups,
                                                  .quarantined = {}};

            const auto plan = Build(validator, request);
            REQUIRE(plan.HasPromotion());
            CHECK(plan.RequiresUserConfirmation());
            CHECK(plan.decisionReason == SaveSlotRecoveryDecisionReason::IncompatibleCurrentRequiresConfirmation);
        }

        TEST_CASE("Current recovery evidence is protected before bounded quarantine cleanup", "[unit][runtime][save][slot-recovery]") {
            FakeValidator validator;
            validator.states = {{10, SaveSlotRecoveryValidationState::Corrupt}, {20, SaveSlotRecoveryValidationState::Corrupt}};
            const auto current = Artifact(90, 10);
            const std::vector backups{Artifact(1, 20), Artifact(2, 30)};
            const std::vector quarantined{Artifact(2, 1), Artifact(3, 2)};
            const SaveSlotRecoveryPolicy policy{.maximumBackups = 1,
                                                .maximumQuarantined = 2,
                                                .maximumObservedArtifacts = 8,
                                                .automaticPromotion = SaveSlotRecoveryAutomaticPolicy::CorruptCurrent};
            const SaveSlotRecoveryRequest request{.slot = Id<SaveGameSlotId>(4),
                                                  .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                  .current = current,
                                                  .backups = backups,
                                                  .quarantined = quarantined};

            const auto plan = Build(validator, request, policy);
            CHECK(Sequences(plan.retainedQuarantine) == std::vector<std::uint64_t>{20, 10});
            REQUIRE(plan.cleanup.size() == 2);
            CHECK(plan.cleanup[0].kind == SaveSlotRecoveryArtifactKind::Quarantine);
            CHECK(plan.cleanup[0].artifact.retentionSequence == 2);
            CHECK(plan.cleanup[1].artifact.retentionSequence == 1);
        }

        TEST_CASE("Malformed recovery bounds and duplicate ordering evidence fail closed", "[unit][runtime][save][slot-recovery]") {
            FakeValidator validator;
            const auto current = Artifact(90, 10);
            const std::vector backups{Artifact(1, 1), Artifact(2, 1)};
            const SaveSlotRecoveryRequest duplicateSequence{.slot = Id<SaveGameSlotId>(4),
                                                            .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                            .current = current,
                                                            .backups = backups,
                                                            .quarantined = {}};
            SaveSlotRecoveryPlanner planner(validator, {});
            const auto duplicateResult = planner.Build(duplicateSequence);
            REQUIRE(duplicateResult.HasError());
            CHECK(duplicateResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryInvalid.code.Value());

            const SaveSlotRecoveryPolicy invalidPolicy{.maximumBackups = 0};
            const std::vector validBackups{Artifact(1, 1)};
            const SaveSlotRecoveryRequest request{.slot = Id<SaveGameSlotId>(4),
                                                  .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                  .current = current,
                                                  .backups = validBackups,
                                                  .quarantined = {}};
            const auto invalidPolicyResult = SaveSlotRecoveryPlanner(validator, invalidPolicy).Build(request);
            REQUIRE(invalidPolicyResult.HasError());
            CHECK(invalidPolicyResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryInvalid.code.Value());
        }

        TEST_CASE("Recovery rejects invalid bounds and incomplete ordering evidence", "[unit][runtime][save][slot-recovery]") {
            FakeValidator validator;
            const auto current = Artifact(90, 10);
            const SaveSlotRecoveryRequest invalidSlot{.slot = {},
                                                      .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                      .current = current,
                                                      .backups = {},
                                                      .quarantined = {}};
            const auto invalidSlotResult = SaveSlotRecoveryPlanner(validator, {}).Build(invalidSlot);
            REQUIRE(invalidSlotResult.HasError());
            CHECK(invalidSlotResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryInvalid.code.Value());

            const SaveSlotRecoveryPolicy tightPolicy{.maximumBackups = 1,
                                                     .maximumQuarantined = 1,
                                                     .maximumObservedArtifacts = 2,
                                                     .automaticPromotion = SaveSlotRecoveryAutomaticPolicy::CorruptCurrent};
            const std::vector tooManyBackups{Artifact(1, 1), Artifact(2, 2)};
            const SaveSlotRecoveryRequest tooManyBackupRequest{.slot = Id<SaveGameSlotId>(4),
                                                               .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                               .current = current,
                                                               .backups = tooManyBackups,
                                                               .quarantined = {}};
            const auto tooManyBackupsResult = SaveSlotRecoveryPlanner(validator, tightPolicy).Build(tooManyBackupRequest);
            REQUIRE(tooManyBackupsResult.HasError());
            CHECK(tooManyBackupsResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryLimitExceeded.code.Value());

            const std::vector oneBackup{Artifact(1, 1)};
            const std::vector tooManyQuarantined{Artifact(2, 2), Artifact(3, 3)};
            const SaveSlotRecoveryRequest tooManyQuarantineRequest{.slot = Id<SaveGameSlotId>(4),
                                                                   .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                                   .current = current,
                                                                   .backups = oneBackup,
                                                                   .quarantined = tooManyQuarantined};
            const auto tooManyQuarantinedResult = SaveSlotRecoveryPlanner(validator, tightPolicy).Build(tooManyQuarantineRequest);
            REQUIRE(tooManyQuarantinedResult.HasError());
            CHECK(tooManyQuarantinedResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryLimitExceeded.code.Value());

            const auto zeroCurrentResult = SaveSlotRecoveryPlanner(validator, {})
                                               .Build(SaveSlotRecoveryRequest{.slot = Id<SaveGameSlotId>(4),
                                                                              .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                                              .current = Artifact(90, 0),
                                                                              .backups = {},
                                                                              .quarantined = {}});
            REQUIRE(zeroCurrentResult.HasError());
            CHECK(zeroCurrentResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryInvalid.code.Value());

            const std::vector zeroSequenceBackup{Artifact(1, 0)};
            const auto zeroBackupResult = SaveSlotRecoveryPlanner(validator, {})
                                              .Build(SaveSlotRecoveryRequest{.slot = Id<SaveGameSlotId>(4),
                                                                             .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                                             .current = current,
                                                                             .backups = zeroSequenceBackup,
                                                                             .quarantined = {}});
            REQUIRE(zeroBackupResult.HasError());
            CHECK(zeroBackupResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryInvalid.code.Value());

            const std::vector zeroSequenceQuarantine{Artifact(1, 0)};
            const auto zeroQuarantineResult = SaveSlotRecoveryPlanner(validator, {})
                                                  .Build(SaveSlotRecoveryRequest{.slot = Id<SaveGameSlotId>(4),
                                                                                 .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                                                 .current = current,
                                                                                 .backups = {},
                                                                                 .quarantined = zeroSequenceQuarantine});
            REQUIRE(zeroQuarantineResult.HasError());
            CHECK(zeroQuarantineResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryInvalid.code.Value());
        }

        TEST_CASE("Recovery preserves interrupted publication evidence and confirmation policy", "[unit][runtime][save][slot-recovery]") {
            FakeValidator validator;
            const auto current = Artifact(90, 10);
            const std::vector backups{Artifact(1, 1)};
            const SaveSlotRecoveryRequest missingCurrent{.slot = Id<SaveGameSlotId>(4),
                                                         .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                         .current = std::nullopt,
                                                         .backups = backups,
                                                         .quarantined = {}};
            const auto missingCurrentResult = SaveSlotRecoveryPlanner(validator, {}).Build(missingCurrent);
            REQUIRE(missingCurrentResult.HasError());
            CHECK(missingCurrentResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryInvalid.code.Value());

            const SaveSlotRecoveryRequest interruptedWithoutCurrent{.slot = Id<SaveGameSlotId>(4),
                                                                    .trigger = SaveSlotRecoveryTrigger::InterruptedPublication,
                                                                    .current = std::nullopt,
                                                                    .backups = {},
                                                                    .quarantined = {}};
            const auto interruptedPlan = Build(validator, interruptedWithoutCurrent);
            CHECK_FALSE(interruptedPlan.currentValidation);
            CHECK(interruptedPlan.decision == SaveSlotRecoveryDecision::NoRecovery);
            CHECK(interruptedPlan.decisionReason == SaveSlotRecoveryDecisionReason::NoValidBackup);

            validator.states = {{10, SaveSlotRecoveryValidationState::Corrupt}};
            const SaveSlotRecoveryRequest interruptedWithCurrent{.slot = Id<SaveGameSlotId>(4),
                                                                 .trigger = SaveSlotRecoveryTrigger::InterruptedPublication,
                                                                 .current = current,
                                                                 .backups = backups,
                                                                 .quarantined = {}};
            const auto confirmationPlan = Build(validator, interruptedWithCurrent);
            REQUIRE(confirmationPlan.HasPromotion());
            CHECK(confirmationPlan.RequiresUserConfirmation());
            CHECK(confirmationPlan.decisionReason == SaveSlotRecoveryDecisionReason::InterruptedPublicationRequiresConfirmation);
        }

        TEST_CASE("Recovery propagates validator failures and rejects trigger disagreements", "[unit][runtime][save][slot-recovery]") {
            FakeValidator validator;
            const auto current = Artifact(90, 10);
            const SaveSlotRecoveryRequest corruptRequest{.slot = Id<SaveGameSlotId>(4),
                                                         .trigger = SaveSlotRecoveryTrigger::CorruptCurrent,
                                                         .current = current,
                                                         .backups = {},
                                                         .quarantined = {}};
            const auto corruptTriggerResult = SaveSlotRecoveryPlanner(validator, {}).Build(corruptRequest);
            REQUIRE(corruptTriggerResult.HasError());
            CHECK(corruptTriggerResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryInvalid.code.Value());

            const SaveSlotRecoveryRequest incompatibleRequest{.slot = Id<SaveGameSlotId>(4),
                                                              .trigger = SaveSlotRecoveryTrigger::IncompatibleCurrent,
                                                              .current = current,
                                                              .backups = {},
                                                              .quarantined = {}};
            const auto incompatibleTriggerResult = SaveSlotRecoveryPlanner(validator, {}).Build(incompatibleRequest);
            REQUIRE(incompatibleTriggerResult.HasError());
            CHECK(incompatibleTriggerResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryInvalid.code.Value());

            validator.failure = MakeError(SaveErrors::SlotRecoveryInvalid);
            const auto validatorFailureResult = SaveSlotRecoveryPlanner(validator, {}).Build(corruptRequest);
            REQUIRE(validatorFailureResult.HasError());
            CHECK(validatorFailureResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryInvalid.code.Value());

            validator.failure.reset();
            validator.returnUnknownState = true;
            const auto unknownStateResult = SaveSlotRecoveryPlanner(validator, {}).Build(corruptRequest);
            REQUIRE(unknownStateResult.HasError());
            CHECK(unknownStateResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryInvalid.code.Value());

            validator.returnUnknownState = false;
            validator.failure = MakeError(SaveErrors::SlotRecoveryInvalid);
            const std::vector backups{Artifact(1, 1)};
            const SaveSlotRecoveryRequest backupFailureRequest{.slot = Id<SaveGameSlotId>(4),
                                                               .trigger = SaveSlotRecoveryTrigger::InterruptedPublication,
                                                               .current = std::nullopt,
                                                               .backups = backups,
                                                               .quarantined = {}};
            const auto backupFailureResult = SaveSlotRecoveryPlanner(validator, {}).Build(backupFailureRequest);
            REQUIRE(backupFailureResult.HasError());
            CHECK(backupFailureResult.ErrorValue().code.Value() == SaveErrors::SlotRecoveryInvalid.code.Value());
        }

        TEST_CASE("Production recovery validation classifies malformed bytes as corrupt without offering promotion",
                  "[unit][runtime][save][slot-recovery]") {
            SaveArchiveRecoveryValidator validator{SaveArchiveReader{}, {}};
            const auto result = validator.Validate(Artifact(1, 1), Id<SaveGameSlotId>(4));
            REQUIRE(result.HasValue());
            CHECK(result.Value().state == SaveSlotRecoveryValidationState::Corrupt);
            REQUIRE(result.Value().diagnostic);
            CHECK(result.Value().diagnostic->code.Value() == SaveErrors::ArchiveEnvelopeInvalid.code.Value());

            const SaveSlotRecoveryArtifact missingMetadata{.metadata = std::nullopt, .archive = {}, .retentionSequence = 1};
            const auto missingMetadataResult = validator.Validate(missingMetadata, Id<SaveGameSlotId>(4));
            REQUIRE(missingMetadataResult.HasValue());
            CHECK(missingMetadataResult.Value().state == SaveSlotRecoveryValidationState::Corrupt);

            const auto wrongScopeResult = validator.Validate(Artifact(1, 1, 3), Id<SaveGameSlotId>(4));
            REQUIRE(wrongScopeResult.HasValue());
            CHECK(wrongScopeResult.Value().state == SaveSlotRecoveryValidationState::Incompatible);

            const SaveSlotRecoveryArtifact missingBytes{.metadata = Entry(4, 1), .archive = {}, .retentionSequence = 1};
            const auto missingBytesResult = validator.Validate(missingBytes, Id<SaveGameSlotId>(4));
            REQUIRE(missingBytesResult.HasValue());
            CHECK(missingBytesResult.Value().state == SaveSlotRecoveryValidationState::Corrupt);
        }
    }  // namespace
}  // namespace Horo::Runtime
