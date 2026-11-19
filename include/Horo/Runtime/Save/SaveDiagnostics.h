#pragma once

/**
 * @file SaveDiagnostics.h
 * @brief Bounded typed Runtime Save failure evidence and partial-data policy.
 */

#include "Horo/Foundation/Diagnostics.h"
#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveNamespace.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace Horo::Runtime {
    /** @brief Save error domain withheld from raw support-bundle entries. */
    inline constexpr std::string_view SaveDiagnosticErrorDomain{"horo.save"};

    inline constexpr std::size_t MaximumSaveDiagnosticContextEntries = 8;
    inline constexpr std::size_t MaximumSaveDiagnosticCauseDepth = 4;
    inline constexpr std::size_t MaximumSavePartialDataFacts = 8;
    inline constexpr std::size_t MaximumSaveDiagnosticMessageBytes = 512;
    inline constexpr std::size_t MaximumPrivateSaveEvidenceBytes = 256;

    /** @brief Closed Runtime Save failure category used without message parsing. */
    enum class SaveFailureCategory : std::uint8_t {
        Validation,
        Compatibility,
        Storage,
        Cancellation,
        Quota,
        Corruption,
        Participant,
        Lifecycle,
        Count,
    };

    /** @brief Explicit owner response to one typed Runtime Save failure. */
    enum class SaveFailureDisposition : std::uint8_t {
        Abort,
        Degrade,
        PreserveOptionalData,
        RequireUserAction,
        Count,
    };

    /** @brief Stable operation stage retained as diagnostic evidence, never scheduling authority. */
    enum class SaveDiagnosticStage : std::uint8_t {
        Admission,
        Capture,
        Serialize,
        FinalizeArchive,
        Storage,
        Commit,
        Verify,
        Migrate,
        Restore,
        Participant,
        Shutdown,
        Count,
    };

    /** @brief Distinguishes immediate rejection from sticky terminal operation outcomes. */
    enum class SaveDiagnosticOutcome : std::uint8_t {
        AdmissionRejected,
        Failed,
        Cancelled,
        TooLate,
        Count,
    };

    /** @brief Durable publication knowledge at the observed outcome. */
    enum class SaveDiagnosticCommitOutcome : std::uint8_t {
        NotCommitted,
        Committed,
        Unknown,
        Count,
    };

    /** @brief Closed safe typed diagnostic context vocabulary. */
    enum class SaveDiagnosticContextKey : std::uint8_t {
        Operation,
        Namespace,
        Slot,
        Participant,
        RegistryGeneration,
        NamespaceRevision,
        SlotGeneration,
        ArchiveGeneration,
        Count,
    };

    using SaveDiagnosticContextValue = std::variant<OperationId, SaveNamespaceId, SaveGameSlotId, SaveParticipantId, SlotGenerationId>;

    /** @brief One typed context entry; records require strictly increasing keys. */
    struct SaveDiagnosticContextEntry final {
        SaveDiagnosticContextKey key{SaveDiagnosticContextKey::Operation};
        SaveDiagnosticContextValue value;
    };

    /** @brief Stable optional save-data role for explicit partial-success decisions. */
    enum class SavePartialDataKind : std::uint8_t {
        Participant,
        Thumbnail,
        DisplayMetadata,
        UnknownPreservedChunk,
        Count,
    };

    enum class SaveDataRequirement : std::uint8_t {
        Required,
        Optional,
        Count
    };
    enum class SavePartialDataOutcome : std::uint8_t {
        Present,
        Omitted,
        Preserved,
        Rejected,
        Count
    };

    /** @brief Explicit required/optional data outcome; absence is never inferred from missing entries. */
    struct SavePartialDataFact final {
        SavePartialDataKind kind{SavePartialDataKind::Participant};
        SaveDataRequirement requirement{SaveDataRequirement::Required};
        SavePartialDataOutcome outcome{SavePartialDataOutcome::Present};
        SaveParticipantId participant;
    };

    /** @brief Safe typed identity of one preserved Runtime Save cause node. */
    struct SaveDiagnosticCause final {
        DiagnosticCode code;
        DiagnosticSeverity severity{DiagnosticSeverity::Error};
    };

    /** @brief Bounded proof that private provider/filesystem evidence was discarded. */
    struct SavePrivateEvidenceSummary final {
        std::uint16_t observedBytes{};
        bool observed{};
        bool truncated{};
        bool malformed{};
        constexpr auto operator<=>(const SavePrivateEvidenceSummary &) const noexcept = default;
    };

    /** @brief Complete typed input for constructing one bounded immutable Runtime Save diagnostic. */
    struct SaveDiagnosticRecordInput final {
        SaveFailureDisposition disposition{SaveFailureDisposition::Abort};
        SaveDiagnosticStage stage{SaveDiagnosticStage::Admission};
        SaveDiagnosticOutcome outcome{SaveDiagnosticOutcome::AdmissionRejected};
        SaveDiagnosticCommitOutcome commitOutcome{SaveDiagnosticCommitOutcome::NotCommitted};
        std::span<const SaveDiagnosticContextEntry> context;
        std::span<const SavePartialDataFact> partialData;
        std::string_view privateEvidence;
    };

    /** @brief Immutable backend-neutral Runtime Save failure evidence with no raw save or native data. */
    class SaveDiagnosticRecord final {
    public:
        [[nodiscard]] constexpr SaveFailureCategory Category() const noexcept {
            return category_;
        }

        [[nodiscard]] constexpr SaveFailureDisposition Disposition() const noexcept {
            return disposition_;
        }

        [[nodiscard]] constexpr SaveDiagnosticStage Stage() const noexcept {
            return stage_;
        }

        [[nodiscard]] constexpr SaveDiagnosticOutcome Outcome() const noexcept {
            return outcome_;
        }

        [[nodiscard]] constexpr SaveDiagnosticCommitOutcome CommitOutcome() const noexcept {
            return commitOutcome_;
        }

        [[nodiscard]] const DiagnosticCode &Code() const noexcept {
            return code_;
        }

        [[nodiscard]] constexpr DiagnosticSeverity Severity() const noexcept {
            return severity_;
        }

        [[nodiscard]] const std::string &Message() const noexcept {
            return message_;
        }

        [[nodiscard]] std::span<const SaveDiagnosticContextEntry> Context() const noexcept;
        [[nodiscard]] std::span<const SavePartialDataFact> PartialData() const noexcept;
        [[nodiscard]] std::span<const SaveDiagnosticCause> Causes() const noexcept;

        [[nodiscard]] constexpr SavePrivateEvidenceSummary PrivateEvidence() const noexcept {
            return privateEvidence_;
        }

    private:
        friend Result<SaveDiagnosticRecord> MakeSaveDiagnosticRecord(const Error &, const SaveDiagnosticRecordInput &);

        SaveFailureCategory category_{SaveFailureCategory::Validation};
        SaveFailureDisposition disposition_{SaveFailureDisposition::Abort};
        SaveDiagnosticStage stage_{SaveDiagnosticStage::Admission};
        SaveDiagnosticOutcome outcome_{SaveDiagnosticOutcome::AdmissionRejected};
        SaveDiagnosticCommitOutcome commitOutcome_{SaveDiagnosticCommitOutcome::NotCommitted};
        DiagnosticCode code_;
        DiagnosticSeverity severity_{DiagnosticSeverity::Error};
        std::string message_;
        std::array<SaveDiagnosticContextEntry, MaximumSaveDiagnosticContextEntries> context_{};
        std::size_t contextCount_{};
        std::array<SavePartialDataFact, MaximumSavePartialDataFacts> partialData_{};
        std::size_t partialDataCount_{};
        std::array<SaveDiagnosticCause, MaximumSaveDiagnosticCauseDepth> causes_{};
        std::size_t causeCount_{};
        SavePrivateEvidenceSummary privateEvidence_{};
    };

    /**
     * @brief Projects a Save failure into an error safe for shared diagnostics and product presentation.
     * @param error Internal failure that may contain private or untrusted provider data.
     * @return A fresh error containing canonical Save descriptors and at most MaximumSaveDiagnosticCauseDepth causes.
     * Unknown descriptors become DiagnosticUnsupported. Source text, attached diagnostics and context never transfer.
     * @throws std::bad_alloc If storage for the projected error cannot be allocated.
     */
    [[nodiscard]] Error RedactSaveDiagnosticError(const Error &error);

    /** @brief Returns every canonical Runtime Save descriptor admitted by diagnostics. */
    [[nodiscard]] std::span<const ErrorCodeDescriptor *const> SaveDiagnosticErrorDescriptors() noexcept;

    /**
     * @brief Builds one bounded immutable diagnostic record from an existing typed Runtime Save error.
     * @param error Canonical Runtime Save error; operation text is discarded in favor of its safe descriptor summary.
     * @param input Typed policy, stage, outcome, correlation, partial-data and private-evidence input.
     * @return Owned record or SaveErrors::DiagnosticInvalid/DiagnosticUnsupported.
     */
    [[nodiscard]] Result<SaveDiagnosticRecord> MakeSaveDiagnosticRecord(const Error &error, const SaveDiagnosticRecordInput &input = {});

    /**
     * @brief Validates retained diagnostic generation evidence against current owner facts.
     * @return Success for exact supplied generations, or SaveErrors::DiagnosticCorrelationStale.
     */
    [[nodiscard]] Result<void> ValidateSaveDiagnosticGenerations(const SaveDiagnosticRecord &record, std::uint64_t registryGeneration,
                                                                 std::uint64_t namespaceRevision, const SlotGenerationId &slotGeneration,
                                                                 std::uint64_t archiveGeneration);
}  // namespace Horo::Runtime
