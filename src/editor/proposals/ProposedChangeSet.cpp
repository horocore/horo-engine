#include "Horo/Editor/ProposedChangeSet.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <new>
#include <set>

namespace Horo::Editor::Proposals {
    namespace {
        /** @brief Produces a stable content-free typed failure. */
        template <class T> Result<T> Fail(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Defines the proposal error domain without untrusted path/text disclosure. */
        ErrorCodeDescriptor Describe(const char *code, const char *summary) {
            return {ErrorDomainId{"horo.editor.proposals"}, ErrorCode{code}, ErrorSeverity::Error, summary};
        }

        /** @brief Recognizes the locale-independent ASCII letters allowed in stable identifiers. */
        bool IdentifierLetter(const unsigned char value) noexcept {
            return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
        }

        /** @brief Recognizes the deliberately small punctuation vocabulary for stable identifiers. */
        bool IdentifierPunctuation(const unsigned char value) noexcept {
            switch (value) {
                case '.':
                case '_':
                case '-':
                case ':':
                    return true;
                default:
                    return false;
            }
        }

        /** @brief Recognizes one stable identity character without locale or provider-specific syntax. */
        bool IdentifierCharacter(const unsigned char value) noexcept {
            if (IdentifierLetter(value))
                return true;
            if (value >= '0' && value <= '9')
                return true;
            return IdentifierPunctuation(value);
        }

        /** @brief Admits bounded stable identities, never arbitrary provider payloads. */
        bool Identifier(const std::string_view value) noexcept {
            return !value.empty() && value.size() <= 128 && std::ranges::all_of(value, IdentifierCharacter);
        }

        /** @brief Shares canonical UTF-8 scalar validation while excluding binary source controls. */
        bool Text(const std::string_view value) noexcept {
            return IsValidUtf8ScalarSequence(value) && std::ranges::none_of(value, [](unsigned char c) {
                return c < 32 && c != '\t' && c != '\n' && c != '\r' && c != '\f';
            });
        }

        /** @brief Folds only ASCII for portable alias rejection; physical Unicode alias checks belong to the host. */
        char Lower(const char c) noexcept {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        }

        /** @brief Owns one bounded collision key without changing the canonical source spelling. */
        std::string Fold(std::string_view text) {
            std::string result(text);
            for (auto &c : result)
                c = Lower(c);
            return result;
        }

        /** @brief Compares a portable filename stem with one lowercase reserved name without allocation. */
        bool StemEquals(const std::string_view stem, const std::string_view reserved) noexcept {
            return stem.size() == reserved.size() && std::equal(stem.begin(), stem.end(), reserved.begin(), [](char a, char b) {
                return Lower(a) == b;
            });
        }

        /** @brief Recognizes the numbered Windows COM/LPT device stems. */
        bool NumberedDeviceStem(const std::string_view stem) noexcept {
            if (stem.size() != 4 || stem[3] < '1' || stem[3] > '9')
                return false;
            const auto prefix = stem.substr(0, 3);
            return StemEquals(prefix, "com") || StemEquals(prefix, "lpt");
        }

        /** @brief Checks the portable reserved-name and trailing-character rules for one path segment. */
        bool PortableSegment(const std::string_view segment) noexcept {
            if (segment.back() == '.' || segment.back() == ' ')
                return false;
            const auto stem = segment.substr(0, segment.find('.'));
            if (StemEquals(stem, "con") || StemEquals(stem, "prn") || StemEquals(stem, "aux") || StemEquals(stem, "nul"))
                return false;
            return !NumberedDeviceStem(stem);
        }

        /** @brief Rejects lexical escapes and portable reserved names; no physical containment authority is implied. */
        bool PortablePath(const SourceDocumentId &path) noexcept {
            if (!path.IsValid() || !IsValidUtf8ScalarSequence(path.Value()))
                return false;
            std::string_view remaining = path.Value();
            while (!remaining.empty()) {
                const auto end = remaining.find('/');
                if (!PortableSegment(remaining.substr(0, end)))
                    return false;
                if (end == std::string_view::npos)
                    break;
                remaining.remove_prefix(end + 1);
            }
            return true;
        }

        /** @brief Length-delimits every policy field before hashing. */
        void Append(std::string &out, std::string_view value) {
            out += std::to_string(value.size());
            out += ':';
            out += value;
        }

        /** @brief Reads the original affected source identity for deterministic operation ordering. */
        const SourceDocumentId &Path(const FileOperation &operation) {
            return std::visit([](const auto &value) -> const SourceDocumentId & {
                if constexpr (std::is_same_v<std::decay_t<decltype(value)>, CreateFile>)
                    return value.path;
                else
                    return value.base.path;
            }, operation);
        }

        /** @brief Bounds untrusted base evidence independently from later live-currentness checks. */
        bool ValidBase(const FileBase &base, const Limits &limits) {
            return base.revision && base.diskRevision && base.diskRevision <= base.revision && base.bytes <= limits.maximumTextBytes &&
                   (!base.utf8Bom || base.bytes >= 3) && base.content != Sha256Digest{} && base.diskContent != Sha256Digest{};
        }

        /** @brief Charges one owned source fragment before any encoding scan or copied allocation. */
        Result<void> ChargeText(const std::string_view text, const Limits &limits, std::size_t &total) {
            if (text.size() > limits.maximumTextBytes || text.size() > limits.maximumTotalTextBytes - total)
                return Fail<void>(Errors::Bounds);
            total += text.size();
            return Result<void>::Success();
        }

        /** @brief Charges both sides of one hunk while retaining the original bounds-error precedence. */
        Result<void> ChargeHunk(const TextHunk &hunk, const Limits &limits, std::size_t &total) {
            // Both per-fragment ceilings precede the aggregate accounting, as in the ingress contract.
            if (hunk.before.size() > limits.maximumTextBytes || hunk.after.size() > limits.maximumTextBytes)
                return Fail<void>(Errors::Bounds);
            if (const auto before = ChargeText(hunk.before, limits, total); before.HasError())
                return before;
            return ChargeText(hunk.after, limits, total);
        }

        /** @brief Requires a non-noop hunk with valid source text on both sides. */
        bool HunkTextValid(const TextHunk &hunk) {
            if (hunk.before.empty() && hunk.after.empty())
                return false;
            return hunk.before != hunk.after && Text(hunk.before) && Text(hunk.after);
        }

        /** @brief Checks an original-base range with overflow-safe subtraction and BOM protection. */
        bool HunkRangeValid(const FileBase &base, const TextHunk &hunk) {
            if (hunk.offset < (base.utf8Bom ? 3U : 0U) || hunk.offset > base.bytes)
                return false;
            return hunk.before.size() <= base.bytes - hunk.offset;
        }

        /** @brief Rejects overlapping ranges and ambiguous insertions at a previously used byte offset. */
        bool HunkOrderValid(const TextHunk &hunk, const std::optional<std::uint64_t> priorStart, const std::uint64_t priorEnd) {
            return !priorStart || (hunk.offset >= priorEnd && hunk.offset != *priorStart);
        }

        /** @brief Canonicalizes original-base hunks after finite text, range and ordering admission. */
        Result<void> Hunks(const FileBase &base, std::vector<TextHunk> &hunks, const Limits &limits, std::size_t &total) {
            if (hunks.size() > limits.maximumHunksPerFile)
                return Fail<void>(Errors::Bounds);
            std::ranges::sort(hunks, {}, &TextHunk::offset);
            std::optional<std::uint64_t> priorStart;
            std::uint64_t priorEnd{};
            auto resultBytes = base.bytes;
            std::uint64_t insertedBytes{};
            for (const auto &hunk : hunks) {
                if (const auto charged = ChargeHunk(hunk, limits, total); charged.HasError())
                    return charged;
                if (!HunkTextValid(hunk) || !HunkRangeValid(base, hunk) || !HunkOrderValid(hunk, priorStart, priorEnd))
                    return Fail<void>(Errors::Invalid);
                resultBytes -= hunk.before.size();
                insertedBytes += hunk.after.size();
                priorStart = hunk.offset;
                priorEnd = hunk.offset + hunk.before.size();
            }
            if (insertedBytes > limits.maximumTextBytes - resultBytes)
                return Fail<void>(Errors::Bounds);
            return Result<void>::Success();
        }

        /** @brief Admits attributable proposal metadata independently from its operation candidates. */
        bool DraftMetadataValid(const Draft &draft) {
            return Identifier(draft.id) && draft.revision && Identifier(draft.provenance.provider) && Identifier(draft.provenance.model) &&
                   Identifier(draft.provenance.request) && !draft.files.empty();
        }

        /** @brief Owns only detached candidate path reservations and aggregate text accounting during admission. */
        class DraftAdmission final {
        public:
            DraftAdmission(const ProjectPolicy &policy, const Limits &limits) : policy_(policy), limits_(limits) {}

            Result<void> Admit(FileOperation &operation) {
                if (const auto reserved = Reserve(Path(operation)); reserved.HasError())
                    return reserved;
                return std::visit([this](auto &value) {
                    return AdmitValue(value);
                }, operation);
            }

        private:
            /** @brief Reserves one policy-admitted path, including rename destinations, before any candidate publication. */
            Result<void> Reserve(const SourceDocumentId &path) {
                if (!policy_.Allows(path))
                    return Fail<void>(Errors::PolicyDenied);
                if (!touched_.insert(Fold(path.Value())).second)
                    return Fail<void>(Errors::Invalid);
                return Result<void>::Success();
            }

            /** @brief Admits new file text without implying destination existence or mutation authority. */
            Result<void> AdmitValue(const CreateFile &file) {
                if (const auto charged = ChargeText(file.text, limits_, total_); charged.HasError())
                    return charged;
                return Text(file.text) ? Result<void>::Success() : Fail<void>(Errors::Invalid);
            }

            /** @brief Admits an exact-base edit with at least one meaningful hunk. */
            Result<void> AdmitValue(EditFile &file) {
                if (!ValidBase(file.base, limits_) || file.hunks.empty())
                    return Fail<void>(Errors::Invalid);
                return Hunks(file.base, file.hunks, limits_, total_);
            }

            /** @brief Admits only exact file-base deletion evidence, never recursive directory removal. */
            Result<void> AdmitValue(const DeleteFile &file) const {
                return ValidBase(file.base, limits_) ? Result<void>::Success() : Fail<void>(Errors::Invalid);
            }

            /** @brief Reserves an absent rename destination before validating optional original-base edits. */
            Result<void> AdmitValue(RenameFile &file) {
                if (!ValidBase(file.base, limits_))
                    return Fail<void>(Errors::Invalid);
                if (const auto destination = Reserve(file.destination); destination.HasError())
                    return destination;
                return Hunks(file.base, file.hunks, limits_, total_);
            }

            const ProjectPolicy &policy_;
            const Limits &limits_;
            std::set<std::string> touched_;
            std::size_t total_{};
        };

        /** @brief Publishes no candidate until all operation paths/text are admitted, then canonicalizes their order. */
        Result<void> PrepareOperations(const ProjectPolicy &policy, Draft &draft, const Limits &limits) {
            DraftAdmission admission{policy, limits};
            for (auto &operation : draft.files) {
                const auto admitted = admission.Admit(operation);
                if (admitted.HasError())
                    return admitted;
            }
            std::ranges::sort(draft.files, {}, [](const auto &operation) {
                return Path(operation).Value();
            });
            return Result<void>::Success();
        }

        /** @brief Compares stable file identity and both independently observed revisions. */
        bool SameBaseRevision(const FileBase &base, const FileBase &current) {
            return base.path == current.path && base.revision == current.revision && base.diskRevision == current.diskRevision;
        }

        /** @brief Compares exact current/saved content evidence and the current encoding marker. */
        bool SameBaseContent(const FileBase &base, const FileBase &current) {
            return base.bytes == current.bytes && base.utf8Bom == current.utf8Bom && base.content == current.content &&
                   base.diskContent == current.diskContent;
        }

        /** @brief Bounds all borrowed hunk text before copying/canonicalizing the structural candidate. */
        Result<void> ValidateSnapshotHunkStructure(const FileBase &base, const std::span<const TextHunk> hunks) {
            const Limits limits;
            if (hunks.size() > limits.maximumHunksPerFile)
                return Fail<void>(Errors::Bounds);
            std::size_t inputBytes{};
            for (const auto &hunk : hunks) {
                const auto charged = ChargeHunk(hunk, limits, inputBytes);
                if (charged.HasError())
                    return charged;
            }
            try {
                auto structural = std::vector<TextHunk>{hunks.begin(), hunks.end()};
                std::size_t total{};
                return Hunks(base, structural, limits, total);
            } catch (const std::bad_alloc &) {
                return Fail<void>(Errors::Bounds);
            }
        }

        /** @brief Tests a bounded UTF-8 scalar boundary without reading past the retained text. */
        bool ScalarBoundary(const std::string_view text, const std::uint64_t at) {
            return at == text.size() || (static_cast<unsigned char>(text[at]) & 0xc0U) != 0x80U;
        }

        /** @brief Checks exact removed text and scalar/BOM boundaries against a real immutable snapshot. */
        Result<void> ValidateSnapshotHunk(const FileBase &base, const TextHunk &hunk, const std::string_view text) {
            if (hunk.offset > text.size() || hunk.before.size() > text.size() - hunk.offset)
                return Fail<void>(Errors::Invalid);
            if (hunk.offset < (base.utf8Bom ? 3U : 0U) || !ScalarBoundary(text, hunk.offset) ||
                !ScalarBoundary(text, hunk.offset + hunk.before.size()) || text.substr(hunk.offset, hunk.before.size()) != hunk.before)
                return Fail<void>(Errors::StaleBase);
            return Result<void>::Success();
        }

        /** @brief Checks a nonzero finite reservation against its hard profile ceiling. */
        bool WithinBudget(const std::size_t value, const std::size_t hard) noexcept {
            return value && value <= hard;
        }

        /** @brief Matches a review/application/save receipt to one exact immutable proposal. */
        template <class Receipt> bool ReceiptMatches(const ChangeSet &proposal, const Receipt &receipt) {
            return receipt.proposal == proposal.Values().id && receipt.revision == proposal.Values().revision &&
                   receipt.digest == proposal.Digest();
        }

        /** @brief Checks application operation counts for the declared complete, partial or unmodified outcome. */
        bool ApplicationCountsValid(const ApplicationReceipt &application, const std::size_t count) {
            if (application.state > ApplicationState::PartiallyApplied || application.appliedOperations > count)
                return false;
            switch (application.state) {
                using enum Horo::Editor::Proposals::ApplicationState;
                case AppliedToWorkspace:
                    return application.appliedOperations == count;
                case PartiallyApplied:
                    return application.appliedOperations > 0 && application.appliedOperations < count;
                default:
                    return application.appliedOperations == 0;
            }
        }

        /** @brief A real save outcome requires complete application; NotRequested remains separate data. */
        bool SaveStateValid(const ApplicationState application, const DiskSaveState save) {
            return save <= DiskSaveState::Failed &&
                   (save == DiskSaveState::NotRequested || application == ApplicationState::AppliedToWorkspace);
        }

        /** @brief Checks attributable review data without treating it as a dispatch approval. */
        bool ReviewMatches(const ChangeSet &proposal, const ReviewRecord &review) {
            const bool knownChoice = review.choice == ReviewChoice::Accept || review.choice == ReviewChoice::Reject;
            return ReceiptMatches(proposal, review) && Identifier(review.reviewer) && knownChoice;
        }

        /** @brief Attaches only an exact attributable review to detached audit data. */
        Result<void> AttachAuditReview(AuditRecord &audit, const ChangeSet &proposal, const std::optional<ReviewRecord> &review) {
            if (!review)
                return Result<void>::Success();
            if (!ReviewMatches(proposal, *review))
                return Fail<void>(Errors::StaleBase);
            audit.review = review->choice;
            return Result<void>::Success();
        }

        /** @brief Any observed application requires accepted review; neither datum grants actual mutation authority. */
        bool ApplicationHasReview(const std::optional<ApplicationReceipt> &application, const std::optional<ReviewRecord> &review) {
            if (!application || !application->appliedOperations)
                return true;
            return review && review->choice == ReviewChoice::Accept;
        }

        /** @brief Validates and attaches the independently supplied application/save observations. */
        Result<void> AttachAuditApplication(AuditRecord &audit, const ChangeSet &proposal, const std::optional<ReviewRecord> &review,
                                            const std::optional<ApplicationReceipt> &application,
                                            const std::optional<DiskSaveReceipt> &save) {
            if (!ApplicationHasReview(application, review))
                return Fail<void>(Errors::Invalid);
            if (save && !application)
                return Fail<void>(Errors::Invalid);
            if (application) {
                if (const auto valid = ValidateReceipts(proposal, *application, save); valid.HasError())
                    return valid;
                audit.application = application->state;
                audit.appliedOperations = application->appliedOperations;
            }
            if (save)
                audit.save = save->state;
            return Result<void>::Success();
        }
    }  // namespace

    namespace Errors {
        const ErrorCodeDescriptor Invalid = Describe("proposal.invalid", "Proposal structure or text is invalid.");
        const ErrorCodeDescriptor PolicyDenied = Describe("proposal.policy_denied", "Proposal is outside the captured project policy.");
        const ErrorCodeDescriptor Bounds = Describe("proposal.bounds", "Proposal exceeds its finite reservation.");
        const ErrorCodeDescriptor StaleBase = Describe("proposal.stale_base", "Proposal base no longer matches the source snapshot.");
        const ErrorCodeDescriptor UnsupportedVersion =
            Describe("proposal.unsupported_version", "Proposal envelope version is unsupported.");
        const ErrorCodeDescriptor Malformed = Describe("proposal.malformed", "Proposal envelope is malformed or its commitment differs.");
    }  // namespace Errors

    /** @copydoc Limits::IsValid */
    bool Limits::IsValid() const noexcept {
        const Limits hard;
        return WithinBudget(maximumFiles, hard.maximumFiles) && WithinBudget(maximumHunksPerFile, hard.maximumHunksPerFile) &&
               WithinBudget(maximumTextBytes, hard.maximumTextBytes) && WithinBudget(maximumTotalTextBytes, hard.maximumTotalTextBytes) &&
               WithinBudget(maximumWireBytes, hard.maximumWireBytes);
    }

    /** @copydoc ProjectPolicy::ProjectPolicy */
    ProjectPolicy::ProjectPolicy(std::string project, std::uint64_t revision, std::vector<PathRule> rules, Sha256Digest digest)
        : project_(std::move(project)), revision_(revision), rules_(std::move(rules)), digest_(digest) {}

    /** @copydoc ProjectPolicy::Capture */
    Result<ProjectPolicy> ProjectPolicy::Capture(std::string project, std::uint64_t revision, std::vector<PathRule> rules) {
        if (!Identifier(project) || !revision || rules.empty() || rules.size() > 32)
            return Fail<ProjectPolicy>(Errors::Invalid);
        try {
            std::ranges::sort(rules, {}, [](const auto &rule) {
                return rule.path.Value();
            });
            std::set<std::string> paths;
            std::string identity = "horo.source-proposal.policy.v1";
            Append(identity, project);
            Append(identity, std::to_string(revision));
            for (const auto &rule : rules) {
                if (!PortablePath(rule.path) || !paths.insert(Fold(rule.path.Value())).second)
                    return Fail<ProjectPolicy>(Errors::PolicyDenied);
                Append(identity, rule.path.Value());
                Append(identity, rule.descendants ? "tree" : "file");
            }
            return Result<ProjectPolicy>::Success(
                ProjectPolicy(std::move(project), revision, std::move(rules), ComputeSha256(std::as_bytes(std::span{identity}))));
        } catch (const std::bad_alloc &) {
            return Fail<ProjectPolicy>(Errors::Bounds);
        }
    }

    /** @copydoc ProjectPolicy::Allows */
    bool ProjectPolicy::Allows(const SourceDocumentId &path) const noexcept {
        if (!PortablePath(path))
            return false;
        for (const auto &rule : rules_) {
            const auto &base = rule.path.Value();
            const auto &value = path.Value();
            if ((!rule.descendants && base == value) ||
                (rule.descendants && value.size() > base.size() && value.starts_with(base) && value[base.size()] == '/'))
                return true;
        }
        return false;
    }

    /** @copydoc FileBase::Capture */
    Result<FileBase> FileBase::Capture(const SourceDocumentSnapshot &source) {
        const auto identity = source.Identity();
        if (!identity.IsValid() || identity.key.kind != DocumentKind::Source || !source.Revision() || !source.BaseRevision())
            return Fail<FileBase>(Errors::Invalid);
        return Result<FileBase>::Success({identity.key.source, source.Revision(), source.BaseRevision(), source.Text().size(),
                                          source.Metadata().utf8Bom, ComputeSha256(std::as_bytes(std::span{source.Text()})),
                                          ComputeSha256(std::as_bytes(std::span{source.DiskBase()}))});
    }

    /** @copydoc ChangeSet::ChangeSet */
    ChangeSet::ChangeSet(const ProjectPolicy &policy, Draft draft, Sha256Digest digest)
        : draft_(std::make_shared<const Draft>(std::move(draft))), digest_(digest), policyDigest_(policy.Digest()),
          project_(policy.Project()), policyRevision_(policy.Revision()) {}

    /** @copydoc ChangeSet::Capture */
    Result<ChangeSet> ChangeSet::Capture(const ProjectPolicy &policy, Draft draft, const Limits &limits) {
        if (!limits.IsValid())
            return Fail<ChangeSet>(Errors::Bounds);
        if (!DraftMetadataValid(draft))
            return Fail<ChangeSet>(Errors::Invalid);
        if (draft.files.size() > limits.maximumFiles)
            return Fail<ChangeSet>(Errors::Bounds);
        try {
            if (const auto admitted = PrepareOperations(policy, draft, limits); admitted.HasError())
                return Result<ChangeSet>::Failure(admitted.ErrorValue());
            ChangeSet proposal{policy, std::move(draft), {}};
            const auto encoded = EncodeRecovery(proposal, limits);
            if (encoded.HasError())
                return Result<ChangeSet>::Failure(encoded.ErrorValue());
            proposal.digest_ = ComputeSha256(std::as_bytes(std::span{encoded.Value()}));
            return Result<ChangeSet>::Success(std::move(proposal));
        } catch (const std::bad_alloc &) {
            return Fail<ChangeSet>(Errors::Bounds);
        }
    }

    /** @copydoc ValidateAgainstSnapshot */
    Result<void> ValidateAgainstSnapshot(const FileBase &base, std::span<const TextHunk> hunks, const SourceDocumentSnapshot &current) {
        const auto actual = FileBase::Capture(current);
        if (actual.HasError())
            return Result<void>::Failure(actual.ErrorValue());
        if (!SameBaseRevision(base, actual.Value()) || !SameBaseContent(base, actual.Value()))
            return Fail<void>(Errors::StaleBase);
        if (const auto structure = ValidateSnapshotHunkStructure(base, hunks); structure.HasError())
            return structure;
        for (const auto &hunk : hunks) {
            const auto valid = ValidateSnapshotHunk(base, hunk, current.Text());
            if (valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    /** @copydoc Review */
    Result<ReviewRecord> Review(const ChangeSet &proposal, const ReviewChoice choice, std::string reviewer) {
        if ((choice != ReviewChoice::Accept && choice != ReviewChoice::Reject) || !Identifier(reviewer))
            return Fail<ReviewRecord>(Errors::Invalid);
        return Result<ReviewRecord>::Success(
            {proposal.Values().id, proposal.Values().revision, proposal.Digest(), choice, std::move(reviewer)});
    }

    /** @copydoc ValidateReceipts */
    Result<void> ValidateReceipts(const ChangeSet &proposal, const ApplicationReceipt &application,
                                  const std::optional<DiskSaveReceipt> &save) {
        if (!ReceiptMatches(proposal, application))
            return Fail<void>(Errors::StaleBase);
        if (save && !ReceiptMatches(proposal, *save))
            return Fail<void>(Errors::StaleBase);
        if (!ApplicationCountsValid(application, proposal.Files().size()))
            return Fail<void>(Errors::Invalid);
        if (save && !SaveStateValid(application.state, save->state))
            return Fail<void>(Errors::Invalid);
        return Result<void>::Success();
    }

    /** @copydoc Audit */
    Result<AuditRecord> Audit(const ChangeSet &proposal, const std::optional<ReviewRecord> &review,
                              const std::optional<ApplicationReceipt> &application, const std::optional<DiskSaveReceipt> &save) {
        AuditRecord result{ComputeSha256(std::as_bytes(std::span{proposal.Values().id})), proposal.Values().revision, proposal.Digest(),
                           proposal.PolicyDigest()};
        for (const auto &operation : proposal.Files())
            ++result.operationCounts[operation.index()];
        if (const auto reviewed = AttachAuditReview(result, proposal, review); reviewed.HasError())
            return Result<AuditRecord>::Failure(reviewed.ErrorValue());
        if (const auto applied = AttachAuditApplication(result, proposal, review, application, save); applied.HasError())
            return Result<AuditRecord>::Failure(applied.ErrorValue());
        return Result<AuditRecord>::Success(std::move(result));
    }
}  // namespace Horo::Editor::Proposals
