#include "Horo/Runtime/Save/SaveDiagnostics.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    namespace {
        constexpr std::string_view SaveErrorDomain = "horo.save";

        struct DiagnosticPolicy final {
            const ErrorCodeDescriptor *descriptor;
            SaveFailureCategory category;
        };

        using enum SaveFailureCategory;
        const std::array kPolicies{
            DiagnosticPolicy{&SaveErrors::IdentityInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::IdentityMalformed, Validation},
            DiagnosticPolicy{&SaveErrors::IdentityDuplicate, Validation},
            DiagnosticPolicy{&SaveErrors::ParticipantIdInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::VersionInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::VersionUnsupportedNewer, Compatibility},
            DiagnosticPolicy{&SaveErrors::ParticipantDescriptorInvalid, Participant},
            DiagnosticPolicy{&SaveErrors::ParticipantAdapterMissing, Participant},
            DiagnosticPolicy{&SaveErrors::ParticipantDuplicate, Participant},
            DiagnosticPolicy{&SaveErrors::ParticipantRecordOwnershipDuplicate, Participant},
            DiagnosticPolicy{&SaveErrors::ParticipantRegistryClosed, Lifecycle},
            DiagnosticPolicy{&SaveErrors::ParticipantRegistryCapacityExceeded, Quota},
            DiagnosticPolicy{&SaveErrors::ParticipantRegistryAllocationFailed, Quota},
            DiagnosticPolicy{&SaveErrors::ParticipantDependencyMissing, Participant},
            DiagnosticPolicy{&SaveErrors::ParticipantDependencyCycle, Participant},
            DiagnosticPolicy{&SaveErrors::ParticipantDependencyPhaseIncompatible, Participant},
            DiagnosticPolicy{&SaveErrors::ParticipantRegistryGenerationExhausted, Lifecycle},
            DiagnosticPolicy{&SaveErrors::CaptureContextInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::CaptureRegistryStale, Lifecycle},
            DiagnosticPolicy{&SaveErrors::CaptureRecordInvalid, Participant},
            DiagnosticPolicy{&SaveErrors::CaptureBudgetExceeded, Quota},
            DiagnosticPolicy{&SaveErrors::CaptureRecordDuplicate, Participant},
            DiagnosticPolicy{&SaveErrors::CaptureIncomplete, Participant},
            DiagnosticPolicy{&SaveErrors::CaptureAlreadySealed, Lifecycle},
            DiagnosticPolicy{&SaveErrors::CaptureAllocationFailed, Quota},
            DiagnosticPolicy{&SaveErrors::CaptureAdapterContractInvalid, Participant},
            DiagnosticPolicy{&SaveErrors::ArchiveHeaderInvalid, Corruption},
            DiagnosticPolicy{&SaveErrors::ArchiveManifestInvalid, Corruption},
            DiagnosticPolicy{&SaveErrors::ArchiveEnvelopeInvalid, Corruption},
            DiagnosticPolicy{&SaveErrors::ArchiveContainerInvalid, Corruption},
            DiagnosticPolicy{&SaveErrors::ArchiveEntryInvalid, Corruption},
            DiagnosticPolicy{&SaveErrors::ArchiveCodecUnsupported, Compatibility},
            DiagnosticPolicy{&SaveErrors::ArchiveDecompressionLimitExceeded, Quota},
            DiagnosticPolicy{&SaveErrors::ArchiveNestingLimitExceeded, Quota},
            DiagnosticPolicy{&SaveErrors::ArchiveUnsafeReference, Validation},
            DiagnosticPolicy{&SaveErrors::ArchiveExtensionInvalid, Compatibility},
            DiagnosticPolicy{&SaveErrors::ArchiveAllocationFailed, Quota},
            DiagnosticPolicy{&SaveErrors::ArchiveStringInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::ArchiveMetadataLimitExceeded, Quota},
            DiagnosticPolicy{&SaveErrors::ArchiveDirectoryInvalid, Corruption},
            DiagnosticPolicy{&SaveErrors::ArchiveFramingLimitExceeded, Quota},
            DiagnosticPolicy{&SaveErrors::ArchivePayloadTruncated, Corruption},
            DiagnosticPolicy{&SaveErrors::ArchiveChunkHashMismatch, Corruption},
            DiagnosticPolicy{&SaveErrors::ArchiveIntegrityAlgorithmUnsupported, Compatibility},
            DiagnosticPolicy{&SaveErrors::ArchiveIntegrityCoverageInvalid, Corruption},
            DiagnosticPolicy{&SaveErrors::ArchiveContentHashMismatch, Corruption},
            DiagnosticPolicy{&SaveErrors::CanonicalStateHashMismatch, Corruption},
            DiagnosticPolicy{&SaveErrors::CanonicalCodecInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::CanonicalCodecCorrupt, Corruption},
            DiagnosticPolicy{&SaveErrors::CanonicalCodecLimitExceeded, Quota},
            DiagnosticPolicy{&SaveErrors::CanonicalCodecDuplicate, Validation},
            DiagnosticPolicy{&SaveErrors::CanonicalCodecNonFinite, Validation},
            DiagnosticPolicy{&SaveErrors::CanonicalCodecUtf8Invalid, Validation},
            DiagnosticPolicy{&SaveErrors::CanonicalCodecConfigurationInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::CanonicalCodecAllocationFailed, Quota},
            DiagnosticPolicy{&SaveErrors::ReferenceInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::ReferenceCorrupt, Corruption},
            DiagnosticPolicy{&SaveErrors::ReferenceResolutionInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::SaveRootConfigurationInvalid, Storage},
            DiagnosticPolicy{&SaveErrors::SaveRootPlatformUnsupported, Storage},
            DiagnosticPolicy{&SaveErrors::SaveRootUnavailable, Storage},
            DiagnosticPolicy{&SaveErrors::SaveRootContainmentViolation, Storage},
            DiagnosticPolicy{&SaveErrors::NamespaceInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::NamespaceUnavailable, Lifecycle},
            DiagnosticPolicy{&SaveErrors::NamespaceStale, Lifecycle},
            DiagnosticPolicy{&SaveErrors::SlotMetadataInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::SlotMetadataLimitExceeded, Quota},
            DiagnosticPolicy{&SaveErrors::SlotDisplayMetadataInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::SlotGenerationConflict, Lifecycle},
            DiagnosticPolicy{&SaveErrors::StorageOperationInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::StorageCapabilityUnsupported, Compatibility},
            DiagnosticPolicy{&SaveErrors::StorageResultInvalid, Storage},
            DiagnosticPolicy{&SaveErrors::StorageAllocationFailed, Quota},
            DiagnosticPolicy{&SaveErrors::SlotCommitInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::SlotCommitOutcomeUnknown, Storage},
            DiagnosticPolicy{&SaveErrors::SlotCommitRecoveryFailed, Storage},
            DiagnosticPolicy{&SaveErrors::StoragePolicyInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::StorageDiskFull, Quota},
            DiagnosticPolicy{&SaveErrors::StorageQuotaExceeded, Quota},
            DiagnosticPolicy{&SaveErrors::StoragePermissionDenied, Storage},
            DiagnosticPolicy{&SaveErrors::StorageReadOnly, Storage},
            DiagnosticPolicy{&SaveErrors::StorageVolumeUnavailable, Storage},
            DiagnosticPolicy{&SaveErrors::StorageTransientIo, Storage},
            DiagnosticPolicy{&SaveErrors::StoragePermanentIo, Storage},
            DiagnosticPolicy{&SaveErrors::OperationInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::OperationAllocationFailed, Quota},
            DiagnosticPolicy{&SaveErrors::OperationTransitionInvalid, Lifecycle},
            DiagnosticPolicy{&SaveErrors::OperationCallbackCapacityExceeded, Quota},
            DiagnosticPolicy{&SaveErrors::OperationCallbackInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::OperationCancelled, Cancellation},
            DiagnosticPolicy{&SaveErrors::OperationDeadlineExceeded, Cancellation},
            DiagnosticPolicy{&SaveErrors::OperationAbandoned, Lifecycle},
            DiagnosticPolicy{&SaveErrors::OperationInProgress, Lifecycle},
            DiagnosticPolicy{&SaveErrors::ArbiterInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::ArbiterCapacityExceeded, Quota},
            DiagnosticPolicy{&SaveErrors::CompositionUnsupported, Compatibility},
            DiagnosticPolicy{&SaveErrors::CompositionInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::CompositionCapacityExceeded, Quota},
            DiagnosticPolicy{&SaveErrors::CompositionCancelled, Cancellation},
            DiagnosticPolicy{&SaveErrors::CompositionInjectedFailure, Lifecycle},
            DiagnosticPolicy{&SaveErrors::CompositionObjectMissing, Storage},
            DiagnosticPolicy{&SaveErrors::DiagnosticInvalid, Validation},
            DiagnosticPolicy{&SaveErrors::DiagnosticUnsupported, Validation},
            DiagnosticPolicy{&SaveErrors::DiagnosticCorrelationStale, Lifecycle},
        };

        const auto &Policies() noexcept {
            return kPolicies;
        }

        std::optional<DiagnosticPolicy> PolicyFor(const Error &error) noexcept {
            if (error.domain.Value() != SaveErrorDomain)
                return std::nullopt;
            for (const auto &policy : Policies()) {
                if (error.code.Value() == policy.descriptor->code.Value())
                    return policy;
            }
            return std::nullopt;
        }

        template <typename Identity> bool ValidIdentity(const SaveDiagnosticContextValue &value) noexcept {
            const auto *identity = std::get_if<Identity>(&value);
            return identity != nullptr && identity->IsValid();
        }

        bool ValidNonZeroScalar(const SaveDiagnosticContextValue &value) noexcept {
            const auto *scalar = std::get_if<OperationId>(&value);
            return scalar != nullptr && *scalar != 0;
        }

        bool ValidNamespace(const SaveDiagnosticContextValue &value) noexcept {
            const auto *namespaceId = std::get_if<SaveNamespaceId>(&value);
            return namespaceId != nullptr && namespaceId->IsValid();
        }

        bool ValidParticipant(const SaveDiagnosticContextValue &value) noexcept {
            const auto *participant = std::get_if<SaveParticipantId>(&value);
            return participant != nullptr && participant->IsValid();
        }

        using ContextValidator = bool (*)(const SaveDiagnosticContextValue &);

        bool ContextValueIsValid(const SaveDiagnosticContextEntry &entry) noexcept {
            static const std::array<ContextValidator, static_cast<std::size_t>(SaveDiagnosticContextKey::Count)> validators{
                ValidNonZeroScalar, ValidNamespace,     ValidIdentity<SaveGameSlotId>,   ValidParticipant,
                ValidNonZeroScalar, ValidNonZeroScalar, ValidIdentity<SlotGenerationId>, ValidNonZeroScalar,
            };
            const auto index = static_cast<std::size_t>(entry.key);
            return index < validators.size() && validators[index](entry.value);
        }

        bool HasContextKey(const std::span<const SaveDiagnosticContextEntry> context, const SaveDiagnosticContextKey key) noexcept {
            return std::ranges::any_of(context, [key](const SaveDiagnosticContextEntry &entry) {
                return entry.key == key;
            });
        }

        bool ContextEntriesAreCanonical(const std::span<const SaveDiagnosticContextEntry> context) noexcept {
            for (std::size_t index = 0; index < context.size(); ++index) {
                if (!ContextValueIsValid(context[index]))
                    return false;
                if (index != 0 && context[index].key <= context[index - 1].key)
                    return false;
            }
            return true;
        }

        bool ContextIsValid(const std::span<const SaveDiagnosticContextEntry> context, const SaveDiagnosticOutcome outcome) noexcept {
            using SaveDiagnosticContextKey::Namespace;
            using SaveDiagnosticContextKey::Operation;
            using SaveDiagnosticContextKey::Slot;
            if (context.size() > MaximumSaveDiagnosticContextEntries || !ContextEntriesAreCanonical(context))
                return false;
            const bool hasOperation = HasContextKey(context, Operation);
            if (outcome == SaveDiagnosticOutcome::AdmissionRejected)
                return !hasOperation;
            return hasOperation && HasContextKey(context, Namespace) && HasContextKey(context, Slot);
        }

        bool PartialFactIsValid(const SavePartialDataFact &fact) noexcept {
            if (fact.kind >= SavePartialDataKind::Count || fact.requirement >= SaveDataRequirement::Count ||
                fact.outcome >= SavePartialDataOutcome::Count)
                return false;
            if ((fact.kind == SavePartialDataKind::Participant) != fact.participant.IsValid())
                return false;
            if (fact.requirement == SaveDataRequirement::Required)
                return fact.outcome == SavePartialDataOutcome::Present || fact.outcome == SavePartialDataOutcome::Rejected;
            return fact.outcome != SavePartialDataOutcome::Rejected;
        }

        bool PartialFactsAreCanonical(const std::span<const SavePartialDataFact> facts) noexcept {
            for (std::size_t index = 0; index < facts.size(); ++index) {
                if (!PartialFactIsValid(facts[index]))
                    return false;
                if (index != 0) {
                    const auto key = std::tuple{facts[index].kind, std::string_view{facts[index].participant.Value()}};
                    const auto previous = std::tuple{facts[index - 1].kind, std::string_view{facts[index - 1].participant.Value()}};
                    if (key <= previous)
                        return false;
                }
            }
            return true;
        }

        bool PartialDataIsValid(const std::span<const SavePartialDataFact> facts) noexcept {
            return facts.size() <= MaximumSavePartialDataFacts && PartialFactsAreCanonical(facts);
        }

        bool OutcomeIsValid(const SaveDiagnosticOutcome outcome, const SaveDiagnosticCommitOutcome commit) noexcept {
            if (outcome >= SaveDiagnosticOutcome::Count || commit >= SaveDiagnosticCommitOutcome::Count)
                return false;
            if (outcome == SaveDiagnosticOutcome::AdmissionRejected || outcome == SaveDiagnosticOutcome::Cancelled)
                return commit == SaveDiagnosticCommitOutcome::NotCommitted;
            if (outcome == SaveDiagnosticOutcome::Failed)
                return commit != SaveDiagnosticCommitOutcome::Committed;
            return true;
        }

        bool RequestEnumsAreKnown(const SaveFailureDisposition disposition, const SaveDiagnosticStage stage) noexcept {
            return disposition < SaveFailureDisposition::Count && stage < SaveDiagnosticStage::Count;
        }

        bool DescriptorSummaryIsSafe(const ErrorCodeDescriptor &descriptor) noexcept {
            return !descriptor.summary.empty() && descriptor.summary.size() <= MaximumSaveDiagnosticMessageBytes;
        }

        bool RequestIsValid(const DiagnosticPolicy &policy, const SaveFailureDisposition disposition, const SaveDiagnosticStage stage,
                            const SaveDiagnosticOutcome outcome, const SaveDiagnosticCommitOutcome commitOutcome,
                            const std::span<const SaveDiagnosticContextEntry> context,
                            const std::span<const SavePartialDataFact> partialData) noexcept {
            if (!RequestEnumsAreKnown(disposition, stage) || !OutcomeIsValid(outcome, commitOutcome))
                return false;
            if (!ContextIsValid(context, outcome) || !PartialDataIsValid(partialData))
                return false;
            return DescriptorSummaryIsSafe(*policy.descriptor);
        }

        struct Utf8Lead final {
            std::size_t continuationCount;
            std::uint32_t codepoint;
        };

        std::byte ByteAt(const std::string_view text, const std::size_t index) noexcept {
            return static_cast<std::byte>(static_cast<unsigned char>(text[index]));
        }

        std::optional<Utf8Lead> DecodeUtf8Lead(const std::byte lead) noexcept {
            if ((lead & std::byte{0xe0}) == std::byte{0xc0} && lead >= std::byte{0xc2})
                return Utf8Lead{1, std::to_integer<std::uint8_t>(lead & std::byte{0x1f})};
            if ((lead & std::byte{0xf0}) == std::byte{0xe0})
                return Utf8Lead{2, std::to_integer<std::uint8_t>(lead & std::byte{0x0f})};
            if ((lead & std::byte{0xf8}) == std::byte{0xf0} && lead <= std::byte{0xf4})
                return Utf8Lead{3, std::to_integer<std::uint8_t>(lead & std::byte{0x07})};
            return std::nullopt;
        }

        bool CanonicalScalar(const std::uint32_t codepoint, const std::size_t width) noexcept {
            if (width == 2)
                return codepoint >= 0x800U && !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
            if (width == 3)
                return codepoint >= 0x10000U && codepoint <= 0x10ffffU;
            return true;
        }

        std::optional<std::size_t> DecodeUtf8(const std::string_view text, const std::size_t index) noexcept {
            const auto lead = DecodeUtf8Lead(ByteAt(text, index));
            if (!lead.has_value() || index + lead->continuationCount >= text.size())
                return std::nullopt;
            std::uint32_t codepoint = lead->codepoint;
            if (const bool validContinuation = std::ranges::all_of(text.substr(index + 1, lead->continuationCount),
                                                                   [&](const char encoded) {
                const auto continuation = static_cast<std::byte>(static_cast<unsigned char>(encoded));
                if ((continuation & std::byte{0xc0}) != std::byte{0x80})
                    return false;
                codepoint = (codepoint << 6U) | std::to_integer<std::uint8_t>(continuation & std::byte{0x3f});
                return true;
            });
                !validContinuation)
                return std::nullopt;
            return CanonicalScalar(codepoint, lead->continuationCount) ? std::optional{lead->continuationCount + 1} : std::nullopt;
        }

        bool IsPrintableUtf8(const std::string_view text) noexcept {
            std::size_t index = 0;
            while (index < text.size()) {
                if (const auto value = std::to_integer<std::uint8_t>(ByteAt(text, index)); value < 0x80U) {
                    if (value < 0x20U || value == 0x7fU)
                        return false;
                    ++index;
                } else if (const auto width = DecodeUtf8(text, index); width.has_value()) {
                    index += *width;
                } else {
                    return false;
                }
            }
            return true;
        }

        SavePrivateEvidenceSummary SummarizePrivateEvidence(const std::string_view privateEvidence) noexcept {
            SavePrivateEvidenceSummary summary;
            summary.observed = !privateEvidence.empty();
            summary.truncated = privateEvidence.size() > MaximumPrivateSaveEvidenceBytes;
            summary.observedBytes = static_cast<std::uint16_t>(std::min(privateEvidence.size(), MaximumPrivateSaveEvidenceBytes + 1));
            const auto inspected = privateEvidence.substr(0, std::min(privateEvidence.size(), MaximumPrivateSaveEvidenceBytes));
            summary.malformed = !IsPrintableUtf8(inspected);
            return summary;
        }

        struct CauseProjection final {
            std::array<SaveDiagnosticCause, MaximumSaveDiagnosticCauseDepth> values{};
            std::size_t count{};
        };

        std::optional<CauseProjection> ProjectCauses(const Error &error) {
            CauseProjection projection;
            const Error *cause = error.cause.Get();
            while (cause != nullptr) {
                if (projection.count == MaximumSaveDiagnosticCauseDepth)
                    return std::nullopt;
                const auto policy = PolicyFor(*cause);
                const auto severity = DiagnosticSeverityForError(cause->severity);
                if (!policy.has_value() || !severity.has_value())
                    return std::nullopt;
                projection.values[projection.count++] = {DiagnosticCode{policy->descriptor->code.Value()}, *severity};
                cause = cause->cause.Get();
            }
            return projection;
        }

        template <typename Value>
        const Value *FindContext(const SaveDiagnosticRecord &record, const SaveDiagnosticContextKey key) noexcept {
            for (const auto &entry : record.Context()) {
                if (entry.key == key)
                    return std::get_if<Value>(&entry.value);
            }
            return nullptr;
        }

        template <typename Value>
        bool ContextMatches(const SaveDiagnosticRecord &record, const SaveDiagnosticContextKey key, const Value &expected) noexcept {
            const auto *actual = FindContext<Value>(record, key);
            return actual != nullptr && *actual == expected;
        }

        bool GenerationsAreValid(const std::uint64_t registryGeneration, const std::uint64_t namespaceRevision,
                                 const SlotGenerationId &slotGeneration, const std::uint64_t archiveGeneration) noexcept {
            return registryGeneration != 0 && namespaceRevision != 0 && slotGeneration.IsValid() && archiveGeneration != 0;
        }

        bool GenerationsMatch(const SaveDiagnosticRecord &record, const std::uint64_t registryGeneration,
                              const std::uint64_t namespaceRevision, const SlotGenerationId &slotGeneration,
                              const std::uint64_t archiveGeneration) noexcept {
            using SaveDiagnosticContextKey::ArchiveGeneration;
            using SaveDiagnosticContextKey::NamespaceRevision;
            using SaveDiagnosticContextKey::RegistryGeneration;
            using SaveDiagnosticContextKey::SlotGeneration;
            if (!ContextMatches<OperationId>(record, RegistryGeneration, registryGeneration))
                return false;
            if (!ContextMatches<OperationId>(record, NamespaceRevision, namespaceRevision))
                return false;
            if (!ContextMatches<SlotGenerationId>(record, SlotGeneration, slotGeneration))
                return false;
            return ContextMatches<OperationId>(record, ArchiveGeneration, archiveGeneration);
        }
    }  // namespace

    std::span<const SaveDiagnosticContextEntry> SaveDiagnosticRecord::Context() const noexcept {
        return {context_.data(), contextCount_};
    }

    std::span<const SavePartialDataFact> SaveDiagnosticRecord::PartialData() const noexcept {
        return {partialData_.data(), partialDataCount_};
    }

    std::span<const SaveDiagnosticCause> SaveDiagnosticRecord::Causes() const noexcept {
        return {causes_.data(), causeCount_};
    }

    std::span<const ErrorCodeDescriptor *const> SaveDiagnosticErrorDescriptors() noexcept {
        static const auto descriptors = [] {
            std::array<const ErrorCodeDescriptor *, std::tuple_size_v<std::remove_reference_t<decltype(Policies())>>> values{};
            std::ranges::transform(Policies(), values.begin(), [](const DiagnosticPolicy &policy) {
                return policy.descriptor;
            });
            return values;
        }();
        return descriptors;
    }

    Result<SaveDiagnosticRecord> MakeSaveDiagnosticRecord(const Error &error, const SaveDiagnosticRecordInput &input) {
        const auto policy = PolicyFor(error);
        const auto severity = DiagnosticSeverityForError(error.severity);
        if (!policy.has_value())
            return Result<SaveDiagnosticRecord>::Failure(MakeError(SaveErrors::DiagnosticUnsupported));
        if (!severity.has_value() ||
            !RequestIsValid(*policy, input.disposition, input.stage, input.outcome, input.commitOutcome, input.context, input.partialData))
            return Result<SaveDiagnosticRecord>::Failure(MakeError(SaveErrors::DiagnosticInvalid));

        const auto causes = ProjectCauses(error);
        if (!causes.has_value())
            return Result<SaveDiagnosticRecord>::Failure(MakeError(SaveErrors::DiagnosticInvalid));

        SaveDiagnosticRecord record;
        record.category_ = policy->category;
        record.disposition_ = input.disposition;
        record.stage_ = input.stage;
        record.outcome_ = input.outcome;
        record.commitOutcome_ = input.commitOutcome;
        record.code_ = DiagnosticCode{policy->descriptor->code.Value()};
        record.severity_ = *severity;
        record.message_ = policy->descriptor->summary;
        record.contextCount_ = input.context.size();
        std::ranges::copy(input.context, record.context_.begin());
        record.partialDataCount_ = input.partialData.size();
        std::ranges::copy(input.partialData, record.partialData_.begin());
        record.privateEvidence_ = SummarizePrivateEvidence(input.privateEvidence);
        record.causes_ = causes->values;
        record.causeCount_ = causes->count;
        return Result<SaveDiagnosticRecord>::Success(std::move(record));
    }

    Result<void> ValidateSaveDiagnosticGenerations(const SaveDiagnosticRecord &record, const std::uint64_t registryGeneration,
                                                   const std::uint64_t namespaceRevision, const SlotGenerationId &slotGeneration,
                                                   const std::uint64_t archiveGeneration) {
        if (!GenerationsAreValid(registryGeneration, namespaceRevision, slotGeneration, archiveGeneration) ||
            !GenerationsMatch(record, registryGeneration, namespaceRevision, slotGeneration, archiveGeneration))
            return Result<void>::Failure(MakeError(SaveErrors::DiagnosticCorrelationStale));
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
