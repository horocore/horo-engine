#include "EditorLocalAgentPatchCheckpoint.h"

#include "Horo/Assets/AssetSourcePublication.h"

#include <algorithm>
#include <charconv>
#include <ranges>

namespace Horo::Editor {
    namespace {
        using namespace Proposals;
        using LoadResult = Result<std::optional<EditorLocalAgentPatchReview>>;
        constexpr std::string_view Header = "horo.source-review.v1\n";
        constexpr auto CheckpointPath = ".horo/local/agent/source-review";
        constexpr std::size_t MaximumPaths = 32;
        constexpr std::size_t MaximumBytes = Limits{}.maximumWireBytes + 512U * 1024U;

        /** @brief Reads one canonical decimal field and advances only over the bounded field. */
        std::optional<std::uint64_t> Number(std::string_view &input) {
            const auto end = input.substr(0, 21).find('\n');
            if (end == 0 || end > 20 || (end > 1 && input.front() == '0'))
                return {};
            std::uint64_t number{};
            const auto *last = input.data() + end;
            const auto [next, error] = std::from_chars(input.data(), last, number);
            if (error != std::errc{} || next != last)
                return {};
            input.remove_prefix(end + 1);
            return number;
        }

        /** @brief Reads one length-framed field, including embedded line endings. */
        std::optional<std::string_view> Field(std::string_view &input) {
            const auto size = Number(input);
            if (!size || *size > input.size())
                return {};
            const auto value = input.substr(0, static_cast<std::size_t>(*size));
            input.remove_prefix(value.size());
            return value;
        }

        /** @brief Adds one finite length-framed field to the versioned envelope. */
        void Append(std::string &output, const std::string_view value) {
            output.append(std::to_string(value.size())).push_back('\n');
            output.append(value);
        }

        /** @brief Resolves the editor's existing project root before using the fixed storage location. */
        Result<std::filesystem::path> CanonicalRoot(const std::filesystem::path &root) {
            using Output = Result<std::filesystem::path>;
            if (!root.is_absolute())
                return Output::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
            std::error_code error;
            auto canonical = std::filesystem::canonical(root, error);
            if (error || !std::filesystem::is_directory(canonical, error) || error)
                return Output::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
            return Output::Success(std::move(canonical));
        }

        /** @brief Retires only our predictable, unaliased staging file under the held publication lease. */
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

        /** @brief Restores only exact path rules that contributed to the original policy commitment. */
        std::vector<SourceDocumentId> ReadPaths(std::string_view &input, const std::size_t count) {
            std::vector<SourceDocumentId> result;
            result.reserve(count);
            for (std::size_t index = 0; index != count; ++index) {
                const auto value = Field(input);
                if (!value)
                    return {};
                auto path = SourceDocumentId::Parse(*value);
                if (path.HasError())
                    return {};
                result.push_back(std::move(path).Value());
            }
            return result;
        }

        /** @brief Matches original allowed paths to immutable snapshots explicitly supplied by the current workspace. */
        std::vector<SourcePathObservation> Observe(const std::vector<SourceDocumentId> &paths,
                                                   const std::span<const SourceDocumentSnapshot> sources) {
            std::vector<SourcePathObservation> observations;
            for (const auto &source : sources) {
                const auto path = source.Identity().key.source;
                if (std::ranges::find(paths, path) != paths.end())
                    observations.push_back({path, source});
            }
            return observations;
        }

        /** @brief Holds decoded path rules and views into the two bounded payloads. */
        struct CheckpointEnvelope final {
            std::uint64_t revision;
            std::vector<SourceDocumentId> paths;
            std::string_view proposal;
            std::string_view decisions;
        };

        /** @brief Admits the complete envelope before interpreting either payload. */
        std::optional<CheckpointEnvelope> ReadEnvelope(std::string_view input) {
            if (!input.starts_with(Header))
                return {};
            input.remove_prefix(Header.size());
            const auto revision = Number(input);
            const auto count = Number(input);
            if (!revision || !count || *count == 0 || *count > MaximumPaths)
                return {};
            auto paths = ReadPaths(input, static_cast<std::size_t>(*count));
            if (paths.size() != *count)
                return {};
            const auto proposed = Field(input);
            const auto reviewed = Field(input);
            if (!proposed || !reviewed || !input.empty())
                return {};
            return CheckpointEnvelope{*revision, std::move(paths), *proposed, *reviewed};
        }

        /** @brief Checks exact proposal identity and current source evidence before publishing restored decisions. */
        LoadResult Restore(CheckpointEnvelope envelope, const std::string_view project,
                           const std::span<const SourceDocumentSnapshot> sources) {
            std::vector<PathRule> rules;
            for (const auto &path : envelope.paths)
                rules.push_back({path, false});
            const auto policy = ProjectPolicy::Capture(std::string{project}, envelope.revision, std::move(rules));
            if (policy.HasError())
                return LoadResult::Failure(policy.ErrorValue());
            auto proposal = DecodeRecovery(envelope.proposal, policy.Value());
            if (proposal.HasError())
                return LoadResult::Failure(proposal.ErrorValue());
            auto plan = PlanRecoveredWorkspaceEdits(proposal.Value(), policy.Value(), Observe(envelope.paths, sources));
            if (plan.HasError())
                return LoadResult::Failure(plan.ErrorValue());
            auto decisions = ReviewDecisions::Decode(proposal.Value(), plan.Value(), envelope.decisions);
            if (decisions.HasError())
                return LoadResult::Failure(decisions.ErrorValue());
            auto cards = DescribeEditorPatchFiles(proposal.Value());
            return LoadResult::Success(EditorLocalAgentPatchReview{std::move(proposal).Value(),
                                                                   std::move(plan).Value(),
                                                                   std::move(cards),
                                                                   std::move(decisions).Value(),
                                                                   std::move(envelope.paths),
                                                                   {},
                                                                   0});
        }

        /** @brief Serializes the checkpoint within its on-disk size budget. */
        Result<std::string> EncodeCheckpoint(const EditorPatchCheckpoint &checkpoint) {
            using Output = Result<std::string>;
            if (checkpoint.offeredPaths.empty() || checkpoint.offeredPaths.size() > MaximumPaths)
                return Output::Failure(MakeError(Errors::Invalid));
            const auto proposal = EncodeRecovery(checkpoint.proposal);
            if (proposal.HasError())
                return Output::Failure(proposal.ErrorValue());
            auto bytes = std::string{Header};
            bytes.append(std::to_string(checkpoint.proposal.PolicyRevision())).push_back('\n');
            bytes.append(std::to_string(checkpoint.offeredPaths.size())).push_back('\n');
            for (const auto &path : checkpoint.offeredPaths)
                Append(bytes, path.Value());
            Append(bytes, proposal.Value());
            Append(bytes, checkpoint.decisions.Encode());
            if (bytes.size() > MaximumBytes)
                return Output::Failure(MakeError(Errors::Bounds));
            return Output::Success(std::move(bytes));
        }

        /** @brief Replaces the checkpoint under its exact project-file lease, using atomic publication. */
        Result<void> PublishCheckpoint(DurableFileSystem &files, const std::filesystem::path &root, const std::span<const std::byte> bytes,
                                       const CancellationToken &stop) {
            using Output = Result<void>;
            const auto path = root / CheckpointPath;
            const auto lock = Assets::AssetSourcePublicationLockPath(root, path);
            if (lock.HasError())
                return Output::Failure(lock.ErrorValue());
            const auto lease = files.TryAcquireExclusive(lock.Value(), "editor-source-review");
            if (lease.HasError())
                return Output::Failure(lease.ErrorValue());
            const auto current = Assets::CaptureAssetSource(root, path, MaximumBytes, stop);
            if (current.HasError())
                return Output::Failure(current.ErrorValue());
            if (const auto directory = files.CreateDirectoryPath(path.parent_path(), DirectoryCreationMode::Parents); directory.HasError())
                return Output::Failure(directory.ErrorValue());
            if (const auto stage = RemoveInterruptedStage(files, path); stage.HasError())
                return Output::Failure(stage.ErrorValue());
            const auto saved = Assets::PublishAssetSource(root, path, current.Value().identity, bytes, lease.Value(), files, stop);
            if (saved.HasError())
                return Output::Failure(saved.ErrorValue());
            if (saved.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                return Output::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
            return Output::Success();
        }
    }  // namespace

    /** @copydoc SaveEditorPatchCheckpoint */
    Result<void> SaveEditorPatchCheckpoint(DurableFileSystem &files, const std::filesystem::path &root,
                                           const EditorPatchCheckpoint &checkpoint, const CancellationToken &stop) {
        const auto canonical = CanonicalRoot(root);
        if (canonical.HasError())
            return Result<void>::Failure(canonical.ErrorValue());
        const auto bytes = EncodeCheckpoint(checkpoint);
        if (bytes.HasError())
            return Result<void>::Failure(bytes.ErrorValue());
        return PublishCheckpoint(files, canonical.Value(), std::as_bytes(std::span{bytes.Value()}), stop);
    }

    /** @copydoc LoadEditorPatchCheckpoint */
    LoadResult LoadEditorPatchCheckpoint(const std::filesystem::path &root, const std::string_view project,
                                         const std::span<const SourceDocumentSnapshot> sources, const CancellationToken &stop) {
        const auto canonical = CanonicalRoot(root);
        if (canonical.HasError())
            return LoadResult::Failure(canonical.ErrorValue());
        const auto captured = Assets::CaptureAssetSource(canonical.Value(), canonical.Value() / CheckpointPath, MaximumBytes, stop);
        if (captured.HasError())
            return LoadResult::Failure(captured.ErrorValue());
        if (!captured.Value().identity.exists)
            return LoadResult::Success(std::nullopt);
        const auto &bytes = captured.Value().bytes;
        if (bytes.empty())
            return LoadResult::Failure(MakeError(Errors::Malformed));
        auto envelope = ReadEnvelope({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
        if (!envelope)
            return LoadResult::Failure(MakeError(Errors::Malformed));
        return Restore(std::move(*envelope), project, sources);
    }
}  // namespace Horo::Editor
