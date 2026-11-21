#pragma once
/** @file ProposedChangeSet.h @brief Inert, bounded untrusted source proposals; review never grants mutation authority. */
#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Foundation/Sha256.h"

#include <array>
#include <deque>
#include <span>
#include <variant>

namespace Horo::Editor::Proposals {
    inline constexpr std::uint32_t WireVersion = 1;

    namespace Errors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor PolicyDenied;
        extern const ErrorCodeDescriptor Bounds;
        extern const ErrorCodeDescriptor StaleBase;
        extern const ErrorCodeDescriptor UnsupportedVersion;
        extern const ErrorCodeDescriptor Malformed;
    }  // namespace Errors

    /** @brief Finite preparation/serialization budgets; callers can narrow but never enlarge defaults. */
    struct Limits final {
        std::size_t maximumFiles{64};
        std::size_t maximumHunksPerFile{256};
        std::size_t maximumTextBytes{8U * 1024U * 1024U};
        std::size_t maximumTotalTextBytes{16U * 1024U * 1024U};
        std::size_t maximumWireBytes{40U * 1024U * 1024U};
        /** @brief Checks finite host ceilings against the fixed hard profile. @return True for admitted limits. */
        bool IsValid() const noexcept;
    };

    /** @brief Host-captured lexical policy rule; descendants are separated by '/', never a string-prefix alias. */
    struct PathRule final {
        SourceDocumentId path;
        bool descendants{};
    };

    /** @brief Immutable explicit project policy; no filesystem reads, trust grants or ambient discovery.
     * @details Paths are lexical project identities. The later mutation owner must recheck current policy,
     * physical containment/symlinks, destination absence and base revisions immediately before applying/saving.
     */
    class ProjectPolicy final {
    public:
        /** @brief Captures a finite host policy. @param project Stable project identity. @param revision Host policy revision.
         * @param rules Explicit allowed files/subtrees; no default wildcard. @return Policy or typed invalid/bounds failure. */
        static Result<ProjectPolicy> Capture(std::string project, std::uint64_t revision, std::vector<PathRule> rules);
        /** @brief Tests a canonical source identity against this policy without I/O. @param path Candidate.
         * @return True only for a portable path admitted by an explicit rule. */
        bool Allows(const SourceDocumentId &path) const noexcept;

        /** @brief Reads captured project identity. @return Immutable stable identity. */
        const std::string &Project() const noexcept {
            return project_;
        }

        /** @brief Reads captured policy revision. @return Nonzero host revision. */
        std::uint64_t Revision() const noexcept {
            return revision_;
        }

        /** @brief Reads canonical policy identity. @return Digest of project, revision and complete rules. */
        Sha256Digest Digest() const noexcept {
            return digest_;
        }

    private:
        /** @brief Retains an admitted explicit policy and its canonical identity. */
        ProjectPolicy(std::string project, std::uint64_t revision, std::vector<PathRule> rules, Sha256Digest digest);
        std::string project_;
        std::uint64_t revision_{};
        std::vector<PathRule> rules_;
        Sha256Digest digest_;
    };

    /** @brief Untrusted exact file-base evidence; a restored revision is observation, never a live session grant. */
    struct FileBase final {
        SourceDocumentId path;
        std::uint64_t revision{};
        std::uint64_t diskRevision{};
        std::uint64_t bytes{};
        bool utf8Bom{};
        Sha256Digest content;
        Sha256Digest diskContent;
        /** @brief Copies real immutable document evidence. @param source Existing source lease.
         * @return Base or invalid inert/non-source snapshot error; no session mutation. */
        static Result<FileBase> Capture(const SourceDocumentSnapshot &source);
    };

    /** @brief Provider-neutral UTF-8 byte hunk against the original base; ranges never shift between hunks. */
    struct TextHunk final {
        std::uint64_t offset{};
        std::string before;
        std::string after;
    };

    /** @brief Creates an absent destination; actual absence is rechecked only by the later mutation owner. */
    struct CreateFile final {
        SourceDocumentId path;
        std::string text;
    };

    struct EditFile final {
        FileBase base;
        std::vector<TextHunk> hunks;
    }; /**< Edits one exact source base. */

    struct DeleteFile final {
        FileBase base;
    }; /**< Deletes one exact source base, never a recursive directory. */

    struct RenameFile final {
        FileBase base;
        SourceDocumentId destination; /**< Must be absent; overwrite is not expressible. */
        std::vector<TextHunk> hunks;  /**< Optional edits refer to the original source base. */
    };

    using FileOperation = std::variant<CreateFile, EditFile, DeleteFile, RenameFile>;

    /** @brief Bounded attributable identities, excluding prompts, transcripts, provider patches and credentials. */
    struct Provenance final {
        std::string provider;
        std::string model;
        std::string request;
    };

    /** @brief Identity assigned by the host for one model answer, independently of its content. */
    struct ProposalIdentity final {
        std::string id;
        std::uint64_t revision{};
        Provenance provenance;
    };

    /** @brief Untrusted ingress values; only ChangeSet::Capture produces an admitted immutable proposal. */
    struct Draft final {
        std::string id;
        std::uint64_t revision{};
        Provenance provenance;
        std::vector<FileOperation> files;
    };

    /** @brief Immutable typed proposal; construction, parsing and review have no buffer/filesystem authority. */
    class ChangeSet final {
    public:
        /** @brief Admits a complete canonical proposal under one captured policy.
         * @param policy Host policy. @param draft Untrusted owned candidates. @param limits Finite host ceilings.
         * @return Complete immutable proposal or typed zero-publication failure. */
        static Result<ChangeSet> Capture(const ProjectPolicy &policy, Draft draft, const Limits &limits = {});

        /** @brief Reads complete immutable operation values. @return Borrowed canonical operation span. */
        std::span<const FileOperation> Files() const noexcept {
            return draft_->files;
        }

        /** @brief Reads the immutable source draft. @return Borrowed admitted values, never a mutable proposal. */
        const Draft &Values() const noexcept {
            return *draft_;
        }

        /** @brief Reads the complete canonical proposal commitment. @return Stable SHA-256 digest. */
        Sha256Digest Digest() const noexcept {
            return digest_;
        }

        /** @brief Reads the bound project policy. @return Captured policy digest. */
        Sha256Digest PolicyDigest() const noexcept {
            return policyDigest_;
        }

        /** @brief Reads stable project identity. @return Captured project. */
        const std::string &Project() const noexcept {
            return project_;
        }

        /** @brief Reads policy revision. @return Captured revision. */
        std::uint64_t PolicyRevision() const noexcept {
            return policyRevision_;
        }

    private:
        /** @brief Owns admitted immutable values without retaining a mutable owner or filesystem service. */
        ChangeSet(const ProjectPolicy &policy, Draft draft, Sha256Digest digest);
        std::shared_ptr<const Draft> draft_;
        Sha256Digest digest_;
        Sha256Digest policyDigest_;
        std::string project_;
        std::uint64_t policyRevision_{};
    };

    /** @brief Checks real source text/base identity and UTF-8 hunk boundaries without applying anything.
     * @param base Captured proposal base. @param hunks Original-base ranges. @param current Actual source lease.
     * @return Success or stale/invalid failure; no buffer, history, save-base or disk mutation. */
    Result<void> ValidateAgainstSnapshot(const FileBase &base, std::span<const TextHunk> hunks, const SourceDocumentSnapshot &current);

    /** @brief Workspace-owned evidence for a path without a readable source snapshot. */
    enum class SourcePathState : std::uint8_t {
        Absent,   /**< The workspace inspected this exact spelling and found no entry. */
        Occupied, /**< The path is in use, including by a directory or an unreadable source. */
        Moved     /**< The workspace observed that the original file moved away. */
    };

    /** @brief One exact workspace observation. Omitted paths have unknown availability. */
    struct SourcePathObservation final {
        SourceDocumentId path;
        std::variant<SourcePathState, SourceDocumentSnapshot> evidence;
    };

    /** @brief The first conflicting prerequisite of one file operation. */
    enum class PlanConflictReason : std::uint8_t {
        Unobserved,
        SourceMissing,
        SourceMoved,
        SourceUnavailable,
        SourceStale,
        SourceChangedOnDisk,
        DestinationOccupied,
        OverlappingPaths
    };

    /** @brief Conflict reason and affected path for one operation in the proposal. */
    struct PlanConflict final {
        SourceDocumentId path;
        PlanConflictReason reason;
    };

    /** @brief Selects the evidence a detached review preview uses to match its original source. */
    enum class ReviewSourceMatch : std::uint8_t {
        CurrentRevision,
        RecoveredContent
    };

    /** @brief Detached preview. The original immutable lease is empty for creates and the result is absent for deletes. */
    struct FilePreview final {
        SourceDocumentSnapshot before;
        std::optional<std::string> after;
        ReviewSourceMatch sourceMatch{ReviewSourceMatch::CurrentRevision};

        /** @brief Checks source identity, freshness, both content digests and exact hunk text.
         * @param base Original proposal evidence. @param hunks Ranges against the original source.
         * @return Success or a typed validation failure.
         * @details A recovered review can match byte-identical source across editor sessions with different revision counters.
         * This evidence only supports preview and review.
         */
        [[nodiscard]] Result<void> ValidateSource(const FileBase &base, std::span<const TextHunk> hunks) const;
    };

    /** @brief A read-only projection bound to the exact proposal digest. Entries follow ChangeSet::Files() order. */
    struct WorkspaceEditPlan final {
        Sha256Digest proposal;
        std::vector<std::variant<FilePreview, PlanConflict>> entries;
    };

    /** @brief Projects an admitted proposal onto an explicitly observed workspace without performing I/O.
     * @param proposal Immutable proposal being reviewed. @param policy Current captured policy for the workspace.
     * @param observations Current workspace observations for source and destination paths. The host supplies absence and disk evidence.
     * @param limits Finite per-file, aggregate text and operation limits.
     * @return A deterministic preview or conflict for each operation, or a typed policy, invalid-input or budget failure.
     * @details Retained previews can become stale. Application must revalidate permissions, paths and revisions against the owner.
     */
    Result<WorkspaceEditPlan> PlanWorkspaceEdits(const ChangeSet &proposal, const ProjectPolicy &policy,
                                                 std::span<const SourcePathObservation> observations, const Limits &limits = {});

    /** @brief Projects an exact restored proposal against current source content and saved-base evidence.
     * @param proposal Exact decoded proposal. @param policy Policy matching the persisted proposal digest.
     * @param observations Current workspace evidence. @param limits Finite per-file and aggregate limits.
     * @return Read-only review previews or conflicts, with no application or save authority.
     */
    Result<WorkspaceEditPlan> PlanRecoveredWorkspaceEdits(const ChangeSet &proposal, const ProjectPolicy &policy,
                                                          std::span<const SourcePathObservation> observations, const Limits &limits = {});

    enum class ReviewChoice : std::uint8_t {
        Accept,
        Reject
    };

    /** @brief Review data only; Accept is not AgentPolicy approval, application or durable save. */
    struct ReviewRecord final {
        std::string proposal;
        std::uint64_t revision{};
        Sha256Digest digest;
        ReviewChoice choice{ReviewChoice::Reject};
        std::string reviewer;
    };

    /** @brief Records exact-proposal review without invoking any mutation owner.
     * @param proposal Immutable reviewed proposal. @param choice Visible decision. @param reviewer Host-supplied stable identity.
     * @return Review data or invalid decision/identity error. */
    Result<ReviewRecord> Review(const ChangeSet &proposal, ReviewChoice choice, std::string reviewer);

    /** @brief One review decision or the aggregate of several units. Mixed is aggregate-only. */
    enum class ReviewState : std::uint8_t {
        Pending,
        Accepted,
        Rejected,
        Mixed
    };

    /** @brief Canonical proposal coordinates. An empty target selects the whole proposal. */
    struct ReviewTarget final {
        std::optional<std::size_t> file;
        std::optional<std::size_t> hunk;
    };

    enum class SourceTransactionFeedback : std::uint8_t {
        None,
        Applied,
        Undone,
        ApplyFailed,
        UndoFailed,
        Conflict,
        Cancelled,
        Preparing
    };

    struct SourceReviewAudit;

    /** @brief Outcome of the latest actual application, independent of later command feedback. */
    enum class SourceReviewDisposition : std::uint8_t {
        Historical, /**< Restored attribution without outcome evidence. */
        Applied,
        Saved,
        Reverted,
        Stale
    };

    /** @brief Owner-published status for the latest explicit application or group undo. */
    struct SourceTransactionStatus final {
        SourceTransactionFeedback feedback{};
        bool canUndo{};
        std::shared_ptr<const SourceReviewAudit> audit;
        bool writingAudit{};
        bool auditHistoryError{};
        SourceReviewDisposition disposition{SourceReviewDisposition::Historical};
    };

    /** @brief Exact owner receipt and its redacted attribution, returned together after publication. */
    struct SourceReviewApplied final {
        SourceEditBatch batch;
        std::shared_ptr<const SourceReviewAudit> audit;
    };

    /** @brief Detached accepted text and exact immutable leases, suitable for worker-to-editor handoff. */
    class PreparedSourceReview final {
    public:
        /** @brief Captures the full saved bytes on a worker before publication.
         * @param root Authorized project root. @param files Host filesystem authority.
         * @param cancellation Cooperative observer. @return Success or typed refusal.
         */
        Result<void> ObserveDisk(const std::filesystem::path &root, const DurableFileSystem &files, CancellationToken cancellation = {});
        /** @brief Publishes through the source owner after its current lease and disk checks.
         * @param documents Current source owner. @param cancellation Current workspace admission.
         * @return Atomic receipt or failure with all source buffers intact.
         */
        [[nodiscard]] Result<SourceEditBatch> Commit(SourceDocumentService &documents, CancellationToken cancellation = {}) const;
        /** @brief Publishes with native metadata revalidation of the worker's full disk reads.
         * @param documents Current source owner. @param files The same active host filesystem authority.
         * @param cancellation Current workspace admission. @return Atomic receipt or typed refusal.
         */
        [[nodiscard]] Result<SourceReviewApplied> CommitObserved(SourceDocumentService &documents, const DurableFileSystem &files,
                                                                 CancellationToken cancellation = {}) const;

    private:
        friend class ReviewDecisions;

        struct Edit final {
            SourceDocumentSnapshot source;
            std::string replacement;
        };

        std::vector<Edit> edits_;
        SourceBatchDiskEvidence disk_;
        std::shared_ptr<const SourceReviewAudit> audit_;
        /** @brief Builds the borrowed batch inputs and dispatches the requested verification mode. */
        [[nodiscard]] Result<SourceEditBatch> Publish(SourceDocumentService &documents, const DurableFileSystem *files,
                                                      CancellationToken cancellation) const;
    };

    /** @brief Inert decisions on one exact proposal. Edits have per-hunk decisions and structural operations are indivisible. */
    class ReviewDecisions final {
    public:
        /** @brief Creates pending decisions in canonical proposal order. */
        explicit ReviewDecisions(ChangeSet proposal);
        /** @brief Reads the exact selected unit or its aggregate. @return Invalid for unknown units. */
        [[nodiscard]] Result<ReviewState> State(ReviewTarget target = {}) const;
        /** @brief Reports whether at least one proposal unit was accepted. */
        [[nodiscard]] bool HasAcceptedChanges() const noexcept;
        /** @brief Records one atomic decision. Acceptance requires a current, conflict-free plan for every selected operation.
         * @param plan The latest defensive workspace plan. @param target Whole proposal, file or edit hunk.
         * @param choice Explicit review-only choice. @return Success or typed failure, with no partial decisions or source writes.
         */
        Result<void> Decide(const WorkspaceEditPlan &plan, ReviewTarget target, ReviewChoice choice);

        /** @brief Reports whether this session has a decision to undo. */
        [[nodiscard]] bool CanUndo() const noexcept {
            return !undo_.empty();
        }

        /** @brief Restores the previous decision batch against the current plan. @return True if a decision was undone. */
        Result<bool> Undo(const WorkspaceEditPlan &plan);
        /** @brief Validates review and renders only accepted hunks without accessing the editor owner or filesystem.
         * @param policy Exact captured project policy. @param plan Immutable proposal preview and source leases.
         * @param cancellation Cooperative observer. @return Detached candidate or typed refusal.
         */
        [[nodiscard]] Result<PreparedSourceReview> PrepareAccepted(const ProjectPolicy &policy, const WorkspaceEditPlan &plan,
                                                                   CancellationToken cancellation = {}) const;
        /** @brief Applies only accepted text hunks through the current source owner.
         * @param documents Owner of the live source snapshots. @param policy Active captured policy for this proposal.
         * @param plan Exact proposal preview with current source evidence. @param cancellation Cooperative observer.
         * @return Atomic buffer receipt or a typed refusal. Applying never writes disk.
         */
        [[nodiscard]] Result<SourceEditBatch> ApplyAccepted(SourceDocumentService &documents, const ProjectPolicy &policy,
                                                            const WorkspaceEditPlan &plan, CancellationToken cancellation = {}) const;
        /** @brief Encodes the proposal digest and canonical unit states without source bytes or approval authority. */
        [[nodiscard]] std::string Encode() const;
        /** @brief Reconstructs decisions bound to the exact proposal and current plan.
         * @param proposal Exact admitted proposal. @param plan Current workspace plan. @param text Bounded review bytes.
         * @return Detached decisions or a typed parse, identity or freshness failure.
         */
        [[nodiscard]] static Result<ReviewDecisions> Decode(ChangeSet proposal, const WorkspaceEditPlan &plan, std::string_view text);

    private:
        using Units = std::vector<std::vector<ReviewState>>;
        [[nodiscard]] bool Contains(ReviewTarget target) const noexcept;
        [[nodiscard]] bool Matches(const WorkspaceEditPlan &plan) const noexcept;
        [[nodiscard]] bool CanAccept(const WorkspaceEditPlan &plan, std::size_t file) const;
        [[nodiscard]] bool AcceptancesCurrent(const WorkspaceEditPlan &plan, const Units &units) const;
        void Publish(Units units);
        ChangeSet proposal_;
        Units units_;
        std::deque<Units> undo_;
    };

    enum class ApplicationState : std::uint8_t {
        NotApplied,
        AppliedToWorkspace,
        FailedBeforeMutation,
        CancelledBeforeMutation,
        Stale,
        PartiallyApplied
    };

    /** @brief Separate application-owner receipt; the proposal/reviewer cannot manufacture application by reviewing. */
    struct ApplicationReceipt final {
        std::string proposal;
        std::uint64_t revision{};
        Sha256Digest digest;
        ApplicationState state{ApplicationState::NotApplied};
        std::uint32_t appliedOperations{};
    };
    enum class DiskSaveState : std::uint8_t {
        NotRequested,
        Durable,
        VisibleDurabilityUnconfirmed,
        Failed
    };

    /** @brief Separate save-owner receipt; applied buffers do not imply durable source publication. */
    struct DiskSaveReceipt final {
        std::string proposal;
        std::uint64_t revision{};
        Sha256Digest digest;
        DiskSaveState state{DiskSaveState::NotRequested};
    };

    /** @brief Validates receipt identity/state consistency without applying or saving anything.
     * @param proposal Exact proposal. @param application Mutation owner's untrusted receipt. @param save Optional separate save receipt.
     * @return Success for coherent evidence, or stale/invalid receipt failure; this never grants authority. */
    Result<void> ValidateReceipts(const ChangeSet &proposal, const ApplicationReceipt &application,
                                  const std::optional<DiskSaveReceipt> &save = {});

    /** @brief Redacted audit projection; deliberately contains no paths, text, model prompts or credentials. */
    struct AuditRecord final {
        Sha256Digest proposalIdentity; /**< Hash of the untrusted proposal ID; raw provider text is not audit identity. */
        std::uint64_t revision{};
        Sha256Digest digest;
        Sha256Digest policy;
        std::array<std::uint32_t, 4> operationCounts{};
        std::optional<ReviewChoice> review;
        std::optional<ApplicationState> application;
        std::uint32_t appliedOperations{};
        std::optional<DiskSaveState> save;
    };

    /** @brief Revisions of an accepted file, in canonical proposal coordinates. */
    struct SourceReviewRevision final {
        std::uint32_t file{};
        std::uint64_t before{};
        std::uint64_t after{};
    };

    /** @brief Detached application evidence with no paths, source text or provider response content. */
    struct SourceReviewAudit final {
        AuditRecord proposal;
        Sha256Digest decisionDigest;                   /**< Commitment to the complete canonical review. */
        std::vector<std::vector<ReviewState>> choices; /**< Canonical per-file unit decisions, without ranges or text. */
        std::vector<SourceReviewRevision> revisions;
        std::uint64_t historySequence{}; /**< Project history identity, assigned under the history writer lease. */
        SourceReviewDisposition disposition{SourceReviewDisposition::Historical}; /**< Last durably recorded outcome. */
    };

    /** @brief Builds bounded content-free audit data. @param proposal Admitted values. @param review Optional exact review.
     * @param application Optional exact application receipt. @param save Optional distinct save receipt.
     * @return Audit values or stale/malformed review error. */
    Result<AuditRecord> Audit(const ChangeSet &proposal, const std::optional<ReviewRecord> &review = {},
                              const std::optional<ApplicationReceipt> &application = {}, const std::optional<DiskSaveReceipt> &save = {});
    /** @brief Encodes the sole proposal recovery format; does not write storage or serialize approval authority.
     * @param proposal Admitted proposal. @param limits Finite wire bound. @return Canonical versioned UTF-8 envelope or capacity error. */
    Result<std::string> EncodeRecovery(const ChangeSet &proposal, const Limits &limits = {});
    /** @brief Parses untrusted recovery against an explicit CURRENT policy; review/approval/apply authority is never restored.
     * @param text Bounded wire bytes. @param policy Current host policy. @param limits Finite ceilings.
     * @return Newly admitted inert proposal or malformed/version/policy/bounds failure. */
    Result<ChangeSet> DecodeRecovery(std::string_view text, const ProjectPolicy &policy, const Limits &limits = {});

    /** @brief Parses the versioned structured model patch into one inert, fully checked proposal.
     * @param text Untrusted JSON with a version and files array. @param policy Current project path policy.
     * @param identity Host-owned attribution. @param sources Exact already-captured bases offered for this answer.
     * @param limits Finite host ceilings. @return Complete proposal or typed denial with no partial publication.
     * @details Existing operations resolve only against provided live snapshots. The parser never opens or edits
     * documents, reads files, accepts provider-supplied base authority, or retains the raw reply.
     */
    Result<ChangeSet> DecodeStructuredPatch(std::string_view text, const ProjectPolicy &policy, ProposalIdentity identity,
                                            std::span<const SourceDocumentSnapshot> sources, const Limits &limits = {});

    /** @brief Converts unified text diff records into inert operations against exact offered bases.
     * @param text Diff with paired a/ and b/ paths and original/new line ranges.
     * @param policy Current project path policy. @param identity Host-issued proposal attribution.
     * @param sources Immutable offered source snapshots. @param limits Finite patch limits.
     * @return Fully validated create, edit or delete proposal, or a typed rejection.
     * @details Plain unified diff has no rename identity. Explicit rename is admitted by the structured parser.
     * The decoder keeps exact source bytes, checks both hunk coordinates, and performs no filesystem I/O.
     */
    Result<ChangeSet> DecodeUnifiedPatch(std::string_view text, const ProjectPolicy &policy, ProposalIdentity identity,
                                         std::span<const SourceDocumentSnapshot> sources, const Limits &limits = {});
    /** @brief Serializes bounded redacted audit data, independently from recovery text. @param record Audit data.
     * @return Canonical versioned envelope or invalid record error. */
    Result<std::string> EncodeAudit(const AuditRecord &record);
    /** @brief Parses only the redacted versioned audit shape. @param text Untrusted bounded bytes.
     * @return Inert audit evidence, never review or dispatch authority. */
    Result<AuditRecord> DecodeAudit(std::string_view text);
    /** @brief Maximum size of one serialized application audit. */
    inline constexpr std::size_t MaximumSourceReviewAuditBytes = 64U * 1024U;
    /** @brief Serializes one content-free applied review. @param audit Exact applied review evidence.
     * @return A checksummed versioned record or a typed validation error. */
    Result<std::string> EncodeSourceReviewAudit(const SourceReviewAudit &audit);
    /** @brief Decodes only inert applied-review evidence. @param text Untrusted bounded bytes.
     * @return Validated attribution with no source-edit, save, or undo authority. */
    Result<SourceReviewAudit> DecodeSourceReviewAudit(std::string_view text);
}  // namespace Horo::Editor::Proposals
