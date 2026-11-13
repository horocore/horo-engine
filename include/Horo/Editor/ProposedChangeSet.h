#pragma once
/** @file ProposedChangeSet.h @brief Inert, bounded untrusted source proposals; review never grants mutation authority. */
#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Foundation/Sha256.h"

#include <array>
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
    /** @brief Serializes bounded redacted audit data, independently from recovery text. @param record Audit data.
     * @return Canonical versioned envelope or invalid record error. */
    Result<std::string> EncodeAudit(const AuditRecord &record);
    /** @brief Parses only the redacted versioned audit shape. @param text Untrusted bounded bytes.
     * @return Inert audit evidence, never review or dispatch authority. */
    Result<AuditRecord> DecodeAudit(std::string_view text);
}  // namespace Horo::Editor::Proposals
