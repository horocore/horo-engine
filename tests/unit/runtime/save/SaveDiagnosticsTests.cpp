#include "Horo/Runtime/Save/SaveDiagnostics.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <string>
#include <vector>

namespace Horo::Runtime {
    namespace {
        SaveNamespaceId Namespace() {
            return {.product = Test::Id<ProductStorageId>(1),
                    .environment = Test::Id<EnvironmentStorageId>(2),
                    .owner = UserProfileOwner{Test::Id<LocalUserStorageId>(3), Test::Id<GameProfileId>(4)}};
        }

        SaveParticipantId Participant(const std::string_view text = "horo.scene.core.v1") {
            return SaveParticipantId::Parse(text).Value();
        }

        std::array<SaveDiagnosticContextEntry, MaximumSaveDiagnosticContextEntries> FullContext() {
            return {
                SaveDiagnosticContextEntry{SaveDiagnosticContextKey::Operation, OperationId{11}},
                SaveDiagnosticContextEntry{SaveDiagnosticContextKey::Namespace, Namespace()},
                SaveDiagnosticContextEntry{SaveDiagnosticContextKey::Slot, Test::Id<SaveGameSlotId>(5)},
                SaveDiagnosticContextEntry{SaveDiagnosticContextKey::Participant, Participant()},
                SaveDiagnosticContextEntry{SaveDiagnosticContextKey::RegistryGeneration, OperationId{12}},
                SaveDiagnosticContextEntry{SaveDiagnosticContextKey::NamespaceRevision, OperationId{13}},
                SaveDiagnosticContextEntry{SaveDiagnosticContextKey::SlotGeneration, Test::Id<SlotGenerationId>(6)},
                SaveDiagnosticContextEntry{SaveDiagnosticContextKey::ArchiveGeneration, OperationId{14}},
            };
        }

        std::array<SaveDiagnosticContextEntry, 3> RequiredTerminalContext() {
            return {
                SaveDiagnosticContextEntry{SaveDiagnosticContextKey::Operation, OperationId{11}},
                SaveDiagnosticContextEntry{SaveDiagnosticContextKey::Namespace, Namespace()},
                SaveDiagnosticContextEntry{SaveDiagnosticContextKey::Slot, Test::Id<SaveGameSlotId>(5)},
            };
        }

        Result<SaveDiagnosticRecord> Record(const Error &error, const SaveFailureDisposition disposition = SaveFailureDisposition::Abort,
                                            const SaveDiagnosticOutcome outcome = SaveDiagnosticOutcome::AdmissionRejected,
                                            const SaveDiagnosticCommitOutcome commit = SaveDiagnosticCommitOutcome::NotCommitted,
                                            const std::span<const SaveDiagnosticContextEntry> context = {},
                                            const std::span<const SavePartialDataFact> partial = {},
                                            const std::string_view privateEvidence = {}) {
            return MakeSaveDiagnosticRecord(error, {.disposition = disposition,
                                                    .stage = SaveDiagnosticStage::Admission,
                                                    .outcome = outcome,
                                                    .commitOutcome = commit,
                                                    .context = context,
                                                    .partialData = partial,
                                                    .privateEvidence = privateEvidence});
        }

        void RequireFailure(const Result<SaveDiagnosticRecord> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        TEST_CASE("Runtime Save diagnostics classify every canonical failure category", "[save][diagnostics]") {
            const std::array cases{
                std::pair{&SaveErrors::IdentityInvalid, SaveFailureCategory::Validation},
                std::pair{&SaveErrors::VersionUnsupportedNewer, SaveFailureCategory::Compatibility},
                std::pair{&SaveErrors::SaveRootUnavailable, SaveFailureCategory::Storage},
                std::pair{&SaveErrors::StorageOperationInvalid, SaveFailureCategory::Validation},
                std::pair{&SaveErrors::StorageCapabilityUnsupported, SaveFailureCategory::Compatibility},
                std::pair{&SaveErrors::StorageResultInvalid, SaveFailureCategory::Storage},
                std::pair{&SaveErrors::StorageAllocationFailed, SaveFailureCategory::Quota},
                std::pair{&SaveErrors::SlotCommitInvalid, SaveFailureCategory::Validation},
                std::pair{&SaveErrors::SlotCommitOutcomeUnknown, SaveFailureCategory::Storage},
                std::pair{&SaveErrors::SlotCommitRecoveryFailed, SaveFailureCategory::Storage},
                std::pair{&SaveErrors::CompositionCancelled, SaveFailureCategory::Cancellation},
                std::pair{&SaveErrors::CaptureBudgetExceeded, SaveFailureCategory::Quota},
                std::pair{&SaveErrors::ParticipantRegistryAllocationFailed, SaveFailureCategory::Quota},
                std::pair{&SaveErrors::OperationAllocationFailed, SaveFailureCategory::Quota},
                std::pair{&SaveErrors::ArchiveChunkHashMismatch, SaveFailureCategory::Corruption},
                std::pair{&SaveErrors::ParticipantDependencyMissing, SaveFailureCategory::Participant},
                std::pair{&SaveErrors::ParticipantDependencyPhaseIncompatible, SaveFailureCategory::Participant},
                std::pair{&SaveErrors::ParticipantRegistryClosed, SaveFailureCategory::Lifecycle},
            };
            for (const auto &[descriptor, expected] : cases) {
                const auto record = Record(MakeError(*descriptor));
                REQUIRE(record.HasValue());
                REQUIRE(record.Value().Category() == expected);
                REQUIRE(record.Value().Code().Value() == descriptor->code.Value());
                REQUIRE(record.Value().Message() == descriptor->summary);
            }

            for (const auto disposition : {SaveFailureDisposition::Abort, SaveFailureDisposition::Degrade,
                                           SaveFailureDisposition::PreserveOptionalData, SaveFailureDisposition::RequireUserAction}) {
                const auto record = Record(MakeError(SaveErrors::CaptureIncomplete), disposition);
                REQUIRE(record.HasValue());
                REQUIRE(record.Value().Disposition() == disposition);
            }

            for (std::uint8_t value = 0; value < static_cast<std::uint8_t>(SaveDiagnosticStage::Count); ++value) {
                const auto stage = static_cast<SaveDiagnosticStage>(value);
                const auto record = MakeSaveDiagnosticRecord(MakeError(SaveErrors::IdentityInvalid), {.stage = stage});
                REQUIRE(record.HasValue());
                REQUIRE(record.Value().Stage() == stage);
            }
        }

        TEST_CASE("Runtime Save diagnostics accept every declared descriptor and reject unknown sources", "[save][diagnostics]") {
            REQUIRE(SaveDiagnosticErrorDescriptors().size() >= 50);
            for (const auto *descriptor : SaveDiagnosticErrorDescriptors()) {
                CAPTURE(descriptor->code.Value());
                REQUIRE(Record(MakeError(*descriptor)).HasValue());
            }

            auto unknown = MakeError(SaveErrors::IdentityInvalid);
            unknown.code = ErrorCode{"save.future.unknown"};
            RequireFailure(Record(unknown), SaveErrors::DiagnosticUnsupported);
            auto foreign = MakeError(SaveErrors::IdentityInvalid);
            foreign.domain = ErrorDomainId{"horo.platform"};
            RequireFailure(Record(foreign), SaveErrors::DiagnosticUnsupported);
            RequireFailure(MakeSaveDiagnosticRecord(MakeError(SaveErrors::IdentityInvalid),
                                                    {.disposition = static_cast<SaveFailureDisposition>(255)}),
                           SaveErrors::DiagnosticInvalid);
            RequireFailure(MakeSaveDiagnosticRecord(MakeError(SaveErrors::IdentityInvalid),
                                                    {.stage = static_cast<SaveDiagnosticStage>(255)}),
                           SaveErrors::DiagnosticInvalid);
        }

        TEST_CASE("Runtime Save diagnostic outcomes keep admission cancellation and commit knowledge distinct", "[save][diagnostics]") {
            REQUIRE(Record(MakeError(SaveErrors::NamespaceInvalid)).HasValue());
            const auto terminalContext = RequiredTerminalContext();
            REQUIRE(Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::Abort, SaveDiagnosticOutcome::Failed,
                           SaveDiagnosticCommitOutcome::NotCommitted, terminalContext)
                        .HasValue());
            REQUIRE(Record(MakeError(SaveErrors::SaveRootUnavailable), SaveFailureDisposition::RequireUserAction,
                           SaveDiagnosticOutcome::Failed, SaveDiagnosticCommitOutcome::Unknown, terminalContext)
                        .HasValue());
            REQUIRE(Record(MakeError(SaveErrors::CompositionCancelled), SaveFailureDisposition::Abort, SaveDiagnosticOutcome::Cancelled,
                           SaveDiagnosticCommitOutcome::NotCommitted, terminalContext)
                        .HasValue());
            REQUIRE(Record(MakeError(SaveErrors::CompositionCancelled), SaveFailureDisposition::Abort, SaveDiagnosticOutcome::TooLate,
                           SaveDiagnosticCommitOutcome::Committed, terminalContext)
                        .HasValue());

            RequireFailure(Record(MakeError(SaveErrors::IdentityInvalid), SaveFailureDisposition::Abort,
                                  SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::Unknown),
                           SaveErrors::DiagnosticInvalid);
            RequireFailure(Record(MakeError(SaveErrors::CompositionCancelled), SaveFailureDisposition::Abort,
                                  SaveDiagnosticOutcome::Cancelled, SaveDiagnosticCommitOutcome::Unknown, terminalContext),
                           SaveErrors::DiagnosticInvalid);
            RequireFailure(Record(MakeError(SaveErrors::SaveRootUnavailable), SaveFailureDisposition::Abort, SaveDiagnosticOutcome::Failed,
                                  SaveDiagnosticCommitOutcome::Committed, terminalContext),
                           SaveErrors::DiagnosticInvalid);
            RequireFailure(Record(MakeError(SaveErrors::IdentityInvalid), SaveFailureDisposition::Abort,
                                  SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::NotCommitted, terminalContext),
                           SaveErrors::DiagnosticInvalid);
            RequireFailure(Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::Abort, SaveDiagnosticOutcome::Failed,
                                  SaveDiagnosticCommitOutcome::NotCommitted),
                           SaveErrors::DiagnosticInvalid);
        }

        TEST_CASE("Runtime Save diagnostic context is owned bounded typed and canonically ordered", "[save][diagnostics]") {
            auto context = FullContext();
            auto record = Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::Abort, SaveDiagnosticOutcome::Failed,
                                 SaveDiagnosticCommitOutcome::NotCommitted, context);
            REQUIRE(record.HasValue());
            REQUIRE(record.Value().Context().size() == MaximumSaveDiagnosticContextEntries);
            const auto originalSlot = std::get<SaveGameSlotId>(record.Value().Context()[2].value);
            context[2].value = Test::Id<SaveGameSlotId>(99);
            REQUIRE(std::get<SaveGameSlotId>(record.Value().Context()[2].value) == originalSlot);

            std::array<SaveDiagnosticContextEntry, MaximumSaveDiagnosticContextEntries + 1> over{};
            RequireFailure(Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::Abort, SaveDiagnosticOutcome::Failed,
                                  SaveDiagnosticCommitOutcome::NotCommitted, over),
                           SaveErrors::DiagnosticInvalid);
            auto duplicate = RequiredTerminalContext();
            duplicate[2] = {SaveDiagnosticContextKey::Namespace, Namespace()};
            RequireFailure(Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::Abort, SaveDiagnosticOutcome::Failed,
                                  SaveDiagnosticCommitOutcome::NotCommitted, duplicate),
                           SaveErrors::DiagnosticInvalid);
            auto wrongType = RequiredTerminalContext();
            wrongType[0].value = Namespace();
            RequireFailure(Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::Abort, SaveDiagnosticOutcome::Failed,
                                  SaveDiagnosticCommitOutcome::NotCommitted, wrongType),
                           SaveErrors::DiagnosticInvalid);
            auto unknownKey = RequiredTerminalContext();
            unknownKey[0].key = SaveDiagnosticContextKey::Count;
            RequireFailure(Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::Abort, SaveDiagnosticOutcome::Failed,
                                  SaveDiagnosticCommitOutcome::NotCommitted, unknownKey),
                           SaveErrors::DiagnosticInvalid);
        }

        TEST_CASE("Runtime Save partial data facts encode required and optional policy explicitly", "[save][diagnostics]") {
            std::array<SavePartialDataFact, MaximumSavePartialDataFacts> facts{};
            facts[0] = {SavePartialDataKind::Participant, SaveDataRequirement::Required, SavePartialDataOutcome::Rejected,
                        Participant("horo.audio.core.v1")};
            for (std::size_t index = 1; index < facts.size(); ++index) {
                const auto id = std::string{"horo.participant.p"} + std::to_string(index);
                facts[index] = {SavePartialDataKind::Participant, SaveDataRequirement::Optional, SavePartialDataOutcome::Omitted,
                                Participant(id)};
            }
            const auto record = Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::Abort,
                                       SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::NotCommitted, {}, facts);
            REQUIRE(record.HasValue());
            REQUIRE(record.Value().PartialData().size() == MaximumSavePartialDataFacts);
            REQUIRE(record.Value().PartialData()[0].requirement == SaveDataRequirement::Required);

            const std::array optionalFacts{
                SavePartialDataFact{SavePartialDataKind::Thumbnail, SaveDataRequirement::Optional, SavePartialDataOutcome::Omitted, {}},
                SavePartialDataFact{SavePartialDataKind::DisplayMetadata,
                                    SaveDataRequirement::Optional,
                                    SavePartialDataOutcome::Present,
                                    {}},
                SavePartialDataFact{SavePartialDataKind::UnknownPreservedChunk,
                                    SaveDataRequirement::Optional,
                                    SavePartialDataOutcome::Preserved,
                                    {}},
            };
            REQUIRE(Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::PreserveOptionalData,
                           SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::NotCommitted, {}, optionalFacts)
                        .HasValue());
        }

        TEST_CASE("Runtime Save partial data facts reject invalid matrices and bounds", "[save][diagnostics]") {
            std::array<SavePartialDataFact, MaximumSavePartialDataFacts + 1> over{};
            RequireFailure(Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::Abort,
                                  SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::NotCommitted, {}, over),
                           SaveErrors::DiagnosticInvalid);
            const std::array invalidRequired{
                SavePartialDataFact{SavePartialDataKind::Thumbnail, SaveDataRequirement::Required, SavePartialDataOutcome::Omitted, {}}};
            RequireFailure(Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::Abort,
                                  SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::NotCommitted, {}, invalidRequired),
                           SaveErrors::DiagnosticInvalid);
            const std::array invalidOptional{SavePartialDataFact{SavePartialDataKind::DisplayMetadata,
                                                                 SaveDataRequirement::Optional,
                                                                 SavePartialDataOutcome::Rejected,
                                                                 {}}};
            RequireFailure(Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::Abort,
                                  SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::NotCommitted, {}, invalidOptional),
                           SaveErrors::DiagnosticInvalid);
            const std::array missingParticipant{SavePartialDataFact{SavePartialDataKind::Participant,
                                                                    SaveDataRequirement::Optional,
                                                                    SavePartialDataOutcome::Preserved,
                                                                    {}}};
            RequireFailure(Record(MakeError(SaveErrors::CaptureIncomplete), SaveFailureDisposition::PreserveOptionalData,
                                  SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::NotCommitted, {},
                                  missingParticipant),
                           SaveErrors::DiagnosticInvalid);
        }

        TEST_CASE("Runtime Save cause chains preserve typed identities within an exact bound", "[save][diagnostics]") {
            Error exact = MakeError(SaveErrors::ArchiveChunkHashMismatch, "raw archive bytes: secret");
            for (std::size_t index = 0; index < MaximumSaveDiagnosticCauseDepth; ++index)
                exact = WrapError(SaveErrors::CaptureIncomplete, std::move(exact), "private path");
            const auto record = Record(exact);
            REQUIRE(record.HasValue());
            REQUIRE(record.Value().Causes().size() == MaximumSaveDiagnosticCauseDepth);
            REQUIRE(record.Value().Causes().back().code.Value() == SaveErrors::ArchiveChunkHashMismatch.code.Value());
            REQUIRE(record.Value().Message() == SaveErrors::CaptureIncomplete.summary);
            REQUIRE(record.Value().Message().find("private path") == std::string::npos);

            Error over = std::move(exact);
            over = WrapError(SaveErrors::CaptureIncomplete, std::move(over));
            RequireFailure(Record(over), SaveErrors::DiagnosticInvalid);

            auto foreignCause = MakeError(SaveErrors::CaptureIncomplete);
            auto foreign = MakeError(SaveErrors::IdentityInvalid);
            foreign.domain = ErrorDomainId{"native.filesystem"};
            foreignCause = WithCause(std::move(foreignCause), std::move(foreign));
            RequireFailure(Record(foreignCause), SaveErrors::DiagnosticInvalid);

            auto unknownCause = MakeError(SaveErrors::CaptureIncomplete);
            auto unknown = MakeError(SaveErrors::IdentityInvalid);
            unknown.code = ErrorCode{"save.future.unknown"};
            unknownCause = WithCause(std::move(unknownCause), std::move(unknown));
            RequireFailure(Record(unknownCause), SaveErrors::DiagnosticInvalid);
        }

        TEST_CASE("Runtime Save private evidence is discarded with bounded UTF-8-safe facts", "[save][diagnostics]") {
            const std::string exact(MaximumPrivateSaveEvidenceBytes, 'x');
            auto record =
                Record(MakeError(SaveErrors::SaveRootUnavailable, "/home/user/token=secret"), SaveFailureDisposition::RequireUserAction,
                       SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::NotCommitted, {}, {}, exact);
            REQUIRE(record.HasValue());
            REQUIRE(record.Value().PrivateEvidence().observedBytes == MaximumPrivateSaveEvidenceBytes);
            REQUIRE_FALSE(record.Value().PrivateEvidence().truncated);
            REQUIRE(record.Value().Message().find("token") == std::string::npos);

            auto exactMessage = MakeError(SaveErrors::SaveRootUnavailable, std::string(MaximumSaveDiagnosticMessageBytes, 'm'));
            REQUIRE(Record(exactMessage).HasValue());
            exactMessage.message.push_back('m');
            REQUIRE(Record(exactMessage).HasValue());

            const std::string over(MaximumPrivateSaveEvidenceBytes + 1, 'y');
            record = Record(MakeError(SaveErrors::SaveRootUnavailable), SaveFailureDisposition::RequireUserAction,
                            SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::NotCommitted, {}, {}, over);
            REQUIRE(record.HasValue());
            REQUIRE(record.Value().PrivateEvidence().truncated);
            REQUIRE(record.Value().PrivateEvidence().observedBytes == MaximumPrivateSaveEvidenceBytes + 1);

            const std::string malformed{"bad\0utf8", 8};
            record = Record(MakeError(SaveErrors::SaveRootUnavailable), SaveFailureDisposition::RequireUserAction,
                            SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::NotCommitted, {}, {}, malformed);
            REQUIRE(record.HasValue());
            REQUIRE(record.Value().PrivateEvidence().malformed);

            const std::array validUtf8Boundaries{
                std::string{"\xc2\x80", 2},
                std::string{"\xe0\xa0\x80", 3},
                std::string{"\xf0\x90\x80\x80", 4},
                std::string{"\xf4\x8f\xbf\xbf", 4},
            };
            for (const auto &evidence : validUtf8Boundaries) {
                record = Record(MakeError(SaveErrors::SaveRootUnavailable), SaveFailureDisposition::RequireUserAction,
                                SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::NotCommitted, {}, {}, evidence);
                REQUIRE(record.HasValue());
                REQUIRE_FALSE(record.Value().PrivateEvidence().malformed);
            }

            const std::array malformedUtf8{
                std::string{"\x1b", 1},     std::string{"\xc0\x80", 2}, std::string{"\xed\xa0\x80", 3}, std::string{"\xf4\x90\x80\x80", 4},
                std::string{"\xe2\x82", 2},
            };
            for (const auto &evidence : malformedUtf8) {
                record = Record(MakeError(SaveErrors::SaveRootUnavailable), SaveFailureDisposition::RequireUserAction,
                                SaveDiagnosticOutcome::AdmissionRejected, SaveDiagnosticCommitOutcome::NotCommitted, {}, {}, evidence);
                REQUIRE(record.HasValue());
                REQUIRE(record.Value().PrivateEvidence().malformed);
            }
        }

        TEST_CASE("Runtime Save diagnostic generations fail closed and retained records remain unchanged", "[save][diagnostics]") {
            const auto context = FullContext();
            auto record = Record(MakeError(SaveErrors::NamespaceStale), SaveFailureDisposition::Abort, SaveDiagnosticOutcome::Failed,
                                 SaveDiagnosticCommitOutcome::NotCommitted, context);
            REQUIRE(record.HasValue());
            REQUIRE(ValidateSaveDiagnosticGenerations(record.Value(), 12, 13, Test::Id<SlotGenerationId>(6), 14).HasValue());
            REQUIRE(ValidateSaveDiagnosticGenerations(record.Value(), 99, 13, Test::Id<SlotGenerationId>(6), 14).HasError());
            REQUIRE(ValidateSaveDiagnosticGenerations(record.Value(), 12, 99, Test::Id<SlotGenerationId>(6), 14).HasError());
            REQUIRE(ValidateSaveDiagnosticGenerations(record.Value(), 12, 13, Test::Id<SlotGenerationId>(99), 14).HasError());
            REQUIRE(ValidateSaveDiagnosticGenerations(record.Value(), 12, 13, Test::Id<SlotGenerationId>(6), 99).HasError());
            REQUIRE(ValidateSaveDiagnosticGenerations(record.Value(), 0, 13, Test::Id<SlotGenerationId>(6), 14).HasError());

            const auto retainedCode = record.Value().Code().Value();
            auto rejected = context;
            rejected[4].value = OperationId{0};
            RequireFailure(Record(MakeError(SaveErrors::NamespaceStale), SaveFailureDisposition::Abort, SaveDiagnosticOutcome::Failed,
                                  SaveDiagnosticCommitOutcome::NotCommitted, rejected),
                           SaveErrors::DiagnosticInvalid);
            REQUIRE(record.Value().Code().Value() == retainedCode);
        }

        TEST_CASE("Runtime Save diagnostic values own worker-safe headless evidence", "[save][diagnostics]") {
            auto source = MakeError(SaveErrors::CompositionUnsupported, "provider/native/path secret");
            auto future = std::async(std::launch::async, [source] {
                return Record(source);
            });
            source.message.assign(1024, 'x');
            auto record = future.get();
            REQUIRE(record.HasValue());
            REQUIRE(record.Value().Message() == SaveErrors::CompositionUnsupported.summary);
            REQUIRE(record.Value().Context().empty());
            REQUIRE(record.Value().PartialData().empty());
        }
    }  // namespace
}  // namespace Horo::Runtime
