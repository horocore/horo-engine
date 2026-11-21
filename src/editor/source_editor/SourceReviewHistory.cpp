#include "SourceReviewHistory.h"

#include "Horo/Assets/AssetSourcePublication.h"

#include <limits>
#include <new>

namespace Horo::Editor {
    namespace {
        using Audit = Proposals::SourceReviewAudit;
        using Latest = Result<std::shared_ptr<const Audit>>;
        using Sequence = Result<std::uint64_t>;
        constexpr std::string_view HistoryHeader = "horo.source-review-history.v1\n";
        constexpr auto MaximumHistoryBytes =
            HistoryHeader.size() + SourceReviewHistoryCapacity * (Proposals::MaximumSourceReviewAuditBytes + 1);
        constexpr auto HistoryPath = ".horo/local/agent/source-review-history";

        /** @brief Holds validated records and the offset remaining after evicting one oldest record from a full image. */
        struct History final {
            std::optional<Audit> latest;
            std::size_t retainedFrom{HistoryHeader.size()};
            std::size_t latestFrom{HistoryHeader.size()};
        };

        /** @brief Uses the project's physical root for bounded no-follow publication. */
        Result<std::filesystem::path> CanonicalRoot(const std::filesystem::path &root) {
            using Output = Result<std::filesystem::path>;
            std::error_code error;
            if (!root.is_absolute())
                return Output::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
            auto path = std::filesystem::canonical(root, error);
            if (error)
                return Output::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
            return Output::Success(std::move(path));
        }

        /** @brief Gives the bounded owned file capture a temporary byte view. */
        std::string_view Text(const Assets::AssetSourceCapture &capture) noexcept {
            if (capture.bytes.empty())
                return {};
            return {reinterpret_cast<const char *>(capture.bytes.data()), capture.bytes.size()};
        }

        /** @brief Validates every checksum before reusing the existing history byte ranges. */
        Result<History> DecodeHistory(std::string_view bytes, const bool exists) {
            using Output = Result<History>;
            if (!exists)
                return Output::Success({});
            if (!bytes.starts_with(HistoryHeader))
                return Output::Failure(MakeError(Proposals::Errors::Malformed));
            bytes.remove_prefix(HistoryHeader.size());
            const auto firstEnd = bytes.find('\n');
            History history;
            std::size_t count{};
            std::size_t offset = HistoryHeader.size();
            while (!bytes.empty() && count < SourceReviewHistoryCapacity) {
                const auto end = bytes.find('\n');
                if (end == std::string_view::npos)
                    return Output::Failure(MakeError(Proposals::Errors::Malformed));
                auto decoded = Proposals::DecodeSourceReviewAudit(bytes.substr(0, end));
                if (decoded.HasError())
                    return Output::Failure(decoded.ErrorValue());
                if (history.latest && history.latest->historySequence && decoded.Value().historySequence <= history.latest->historySequence)
                    return Output::Failure(MakeError(Proposals::Errors::Malformed));
                history.latestFrom = offset;
                offset += end + 1;
                history.latest = std::move(decoded).Value();
                bytes.remove_prefix(end + 1);
                ++count;
            }
            if (!bytes.empty() || !history.latest)
                return Output::Failure(MakeError(Proposals::Errors::Malformed));
            if (count == SourceReviewHistoryCapacity)
                history.retainedFrom += firstEnd + 1;
            return Output::Success(std::move(history));
        }

        /** @brief Allocates or reuses only an exact, still-current application sequence. */
        Sequence ClaimSequence(const History &history, const Audit &audit, const std::uint64_t expected) {
            const auto latest = history.latest ? history.latest->historySequence : 0;
            if (!expected && latest < std::numeric_limits<std::uint64_t>::max())
                return Sequence::Success(latest + 1);
            if (history.latest && expected && latest == expected && history.latest->proposal.digest == audit.proposal.digest &&
                history.latest->decisionDigest == audit.decisionDigest)
                return Sequence::Success(expected);
            return Sequence::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        }

        /** @brief Once a completed outcome is stored, only an idempotent write may retain it. */
        bool CanRecordOutcome(const History &history, const std::uint64_t sequence, const Proposals::SourceReviewDisposition next) {
            return !sequence || history.latest->disposition == Proposals::SourceReviewDisposition::Applied ||
                   history.latest->disposition == next;
        }

        /** @brief Retires only the reserved, single-link staging file under the held publication lease. */
        Result<void> RemoveInterruptedStage(DurableFileSystem &files, std::filesystem::path path) {
            path += ".source-publication.tmp";
            const auto entry = files.InspectEntry(path);
            if (entry.HasError())
                return Result<void>::Failure(entry.ErrorValue());
            if (!entry.Value())
                return Result<void>::Success();
            if (entry.Value()->kind != FileEntryKind::RegularFile || entry.Value()->hardLinks != 1)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            return files.RemoveDurable(path);
        }

        /** @brief Publishes one history image while its project and destination leases are held. */
        Result<void> PublishHistory(const std::filesystem::path &root, const std::filesystem::path &path, std::string_view bytes,
                                    const Assets::AssetSourceIdentity &expected, DurableFileSystem &files, const CancellationToken &stop) {
            const auto lockPath = Assets::AssetSourcePublicationLockPath(root, path);
            if (lockPath.HasError())
                return Result<void>::Failure(lockPath.ErrorValue());
            const auto lock = files.TryAcquireExclusive(lockPath.Value(), "source-review-history");
            if (lock.HasError())
                return Result<void>::Failure(lock.ErrorValue());
            const auto directory = files.CreateDirectoryPath(path.parent_path(), DirectoryCreationMode::Parents);
            if (directory.HasError())
                return Result<void>::Failure(directory.ErrorValue());
            if (const auto stage = RemoveInterruptedStage(files, path); stage.HasError())
                return stage;
            const auto saved = Assets::PublishAssetSource(root, path, expected, std::as_bytes(std::span{bytes}), lock.Value(), files, stop);
            if (saved.HasError())
                return Result<void>::Failure(saved.ErrorValue());
            if (saved.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ReadSourceReviewHistory */
    Latest ReadSourceReviewHistory(const std::filesystem::path &root, const CancellationToken &stop) {
        try {
            const auto canonical = CanonicalRoot(root);
            if (canonical.HasError())
                return Latest::Failure(canonical.ErrorValue());
            const auto &path = canonical.Value();
            const auto captured = Assets::CaptureAssetSource(path, path / HistoryPath, MaximumHistoryBytes, stop);
            if (captured.HasError())
                return Latest::Failure(captured.ErrorValue());
            auto history = DecodeHistory(Text(captured.Value()), captured.Value().identity.exists);
            if (history.HasError())
                return Latest::Failure(history.ErrorValue());
            auto latest = std::move(history).Value().latest;
            if (!latest)
                return Latest::Success({});
            return Latest::Success(std::make_shared<const Audit>(std::move(*latest)));
        } catch (const std::bad_alloc &) {
            return Latest::Failure(MakeError(Proposals::Errors::Bounds));
        }
    }

    /** @copydoc WriteSourceReviewHistory */
    Sequence WriteSourceReviewHistory(const std::filesystem::path &root, const Audit &audit, const std::uint64_t sequence,
                                      const Proposals::SourceReviewDisposition outcome, const ProjectMutationCoordinator &mutations,
                                      DurableFileSystem &files, const CancellationToken &stop) {
        try {
            const auto canonical = CanonicalRoot(root);
            if (canonical.HasError())
                return Sequence::Failure(canonical.ErrorValue());
            const auto &project = canonical.Value();
            const auto path = project / HistoryPath;
            const auto authority = mutations.TryAcquire({project, ProjectMutationOwner::Autosave, "source-review-history"});
            if (authority.HasError())
                return Sequence::Failure(authority.ErrorValue());
            const auto current = Assets::CaptureAssetSource(project, path, MaximumHistoryBytes, stop);
            if (current.HasError())
                return Sequence::Failure(current.ErrorValue());
            const auto &capture = current.Value();
            const auto existing = Text(capture);
            const auto history = DecodeHistory(existing, capture.identity.exists);
            if (history.HasError())
                return Sequence::Failure(history.ErrorValue());
            const auto id = ClaimSequence(history.Value(), audit, sequence);
            if (id.HasError())
                return id;
            if (!CanRecordOutcome(history.Value(), sequence, outcome))
                return Sequence::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            auto entry = audit;
            entry.historySequence = id.Value();
            entry.disposition = outcome;
            const auto record = Proposals::EncodeSourceReviewAudit(entry);
            if (record.HasError())
                return Sequence::Failure(record.ErrorValue());
            auto bytes = std::string{HistoryHeader};
            if (capture.identity.exists) {
                const auto body = sequence ? existing.substr(HistoryHeader.size(), history.Value().latestFrom - HistoryHeader.size())
                                           : existing.substr(history.Value().retainedFrom);
                bytes.append(body);
            }
            bytes.append(record.Value()).push_back('\n');
            const auto published = PublishHistory(project, path, bytes, capture.identity, files, stop);
            if (published.HasError())
                return Sequence::Failure(published.ErrorValue());
            return id;
        } catch (const std::bad_alloc &) {
            return Sequence::Failure(MakeError(Proposals::Errors::Bounds));
        }
    }
}  // namespace Horo::Editor
