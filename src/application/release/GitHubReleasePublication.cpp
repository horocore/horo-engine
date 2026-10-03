#include "Horo/Release/GitHubReleasePublication.h"

#include "Horo/Release/ReleaseErrors.h"

#include <algorithm>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace Horo::Release {
    namespace {
        [[nodiscard]] bool ValidRepository(const std::string_view repository) {
            if (const auto slash = repository.find('/');
                slash == std::string_view::npos || slash == 0U || slash + 1U == repository.size() ||
                repository.find('/', slash + 1U) != std::string_view::npos || repository.size() > 200U)
                return false;
            return std::ranges::all_of(repository, [](const char character) {
                return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.' ||
                       character == '/';
            });
        }

        [[nodiscard]] bool ValidGitCommit(const std::string_view commit) {
            return commit.size() == 40U && std::ranges::all_of(commit, [](const char character) {
                return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
            });
        }

        [[nodiscard]] std::optional<std::string> ReviewedBody(const ReleasePublicationRequest &request) {
            try {
                const auto notes = nlohmann::json::parse(request.plan.ReleaseNotesSnapshot());
                if (!notes.is_object() || !notes.contains("markdown") || !notes.at("markdown").is_string())
                    return std::nullopt;
                return notes.at("markdown").get<std::string>();
            } catch (const nlohmann::json::exception &) {
                return std::nullopt;
            }
        }

        [[nodiscard]] std::string NormalizeLineEndings(const std::string_view text) {
            std::string normalized;
            normalized.reserve(text.size());
            for (std::size_t index = 0; index < text.size(); ++index) {
                if (text[index] == '\r' && index + 1U < text.size() && text[index + 1U] == '\n')
                    continue;
                normalized.push_back(text[index]);
            }
            return normalized;
        }

        [[nodiscard]] std::string ExpectedTag(const ReleasePublicationRequest &request) {
            return "v" + std::visit([](const auto &version) {
                return FormatReleaseVersion(version.value);
            }, request.manifest.Data().version);
        }

        [[nodiscard]] std::string AssetName(const std::string_view path) {
            std::string name;
            name.reserve(path.size());
            for (const char character : path) {
                if (character == '%')
                    name += "%25";
                else if (character == '/')
                    name += "%2F";
                else
                    name += character;
            }
            return name;
        }

        [[nodiscard]] bool MatchesEvidence(const GitHubReleaseAssetEvidence &remote, const std::string_view name,
                                           const ReleaseArtifactRecord &expected) {
            return remote.name == name && remote.size == expected.size && remote.digest == expected.digest;
        }

        [[nodiscard]] Result<GitHubReleaseIdentity> ExistingRelease(IGitHubReleaseClient &client, const std::string_view repository,
                                                                    const ReleasePublicationRequest &request,
                                                                    const std::string_view receiptId = {}) {
            const auto &source = request.manifest.Data().sourceRevision.value;
            const auto reviewedBody = ReviewedBody(request);
            if (!ValidRepository(repository) || !ValidGitCommit(source) || !reviewedBody ||
                (request.manifest.Data().version != request.plan.Request().version.productVersion))
                return Result<GitHubReleaseIdentity>::Failure(MakeError(ReleaseErrors::PipelineInputChanged));
            const std::string tag = ExpectedTag(request);
            auto found = client.FindExisting(repository, tag);
            if (found.HasError())
                return found;
            if (const auto &release = found.Value(); release.repository != repository || release.tag != tag || release.releaseId == 0U ||
                                                     release.sourceCommit != source ||
                                                     NormalizeLineEndings(release.body) != *reviewedBody ||
                                                     (!receiptId.empty() && std::to_string(release.releaseId) != receiptId))
                return Result<GitHubReleaseIdentity>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            return found;
        }

        [[nodiscard]] ReleaseArtifactRecord ManifestRecord(const ReleaseArtifactManifest &manifest) {
            return {"manifest.json", ReleaseArtifactRole::Notice, manifest.CanonicalJson().size(), manifest.Digest()};
        }
    }  // namespace

    /** @copydoc GitHubReleasePublicationAdapter::GitHubReleasePublicationAdapter */
    GitHubReleasePublicationAdapter::GitHubReleasePublicationAdapter(std::string repository, IGitHubReleaseClient &client)
        : repository_(std::move(repository)), client_(client) {}

    /** @copydoc GitHubReleasePublicationAdapter::Destination */
    ReleaseDestinationId GitHubReleasePublicationAdapter::Destination() const {
        return {"github-releases"};
    }

    /** @copydoc GitHubReleasePublicationAdapter::Upload */
    Result<ReleasePublicationReceipt> GitHubReleasePublicationAdapter::Upload(const ReleasePublicationRequest &request) {
        auto existing = ExistingRelease(client_, repository_, request);
        if (existing.HasError())
            return Result<ReleasePublicationReceipt>::Failure(existing.ErrorValue());
        const auto &release = existing.Value();
        for (const auto &artifact : request.manifest.Artifacts()) {
            auto uploaded = client_.UploadExact(release, AssetName(artifact.path), request.verified.Root() / artifact.path, artifact);
            if (uploaded.HasError())
                return Result<ReleasePublicationReceipt>::Failure(uploaded.ErrorValue());
        }
        const auto metadata = ManifestRecord(request.manifest);
        if (auto uploaded = client_.UploadExact(release, metadata.path, request.verified.Root() / metadata.path, metadata);
            uploaded.HasError())
            return Result<ReleasePublicationReceipt>::Failure(uploaded.ErrorValue());
        return Result<ReleasePublicationReceipt>::Success({Destination(), request.verified.Candidate(), request.verified.ManifestDigest(),
                                                           request.manifest.Artifacts().size(), std::to_string(release.releaseId)});
    }

    /** @copydoc GitHubReleasePublicationAdapter::VerifyRemote */
    Result<void> GitHubReleasePublicationAdapter::VerifyRemote(const ReleasePublicationRequest &request,
                                                               const ReleasePublicationReceipt &receipt) {
        if (receipt.remoteIdentity.empty())
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        auto existing = ExistingRelease(client_, repository_, request, receipt.remoteIdentity);
        if (existing.HasError())
            return Result<void>::Failure(existing.ErrorValue());
        const auto &release = existing.Value();
        for (const auto &artifact : request.manifest.Artifacts()) {
            const std::string name = AssetName(artifact.path);
            auto remote = client_.ReadAsset(release, name);
            if (remote.HasError())
                return Result<void>::Failure(remote.ErrorValue());
            if (!MatchesEvidence(remote.Value(), name, artifact))
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        }
        const auto metadata = ManifestRecord(request.manifest);
        auto remote = client_.ReadAsset(release, metadata.path);
        if (remote.HasError())
            return Result<void>::Failure(remote.ErrorValue());
        if (!MatchesEvidence(remote.Value(), metadata.path, metadata))
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        return Result<void>::Success();
    }

    /** @copydoc GitHubReleasePublicationAdapter::CommitChannel */
    Result<void> GitHubReleasePublicationAdapter::CommitChannel(const ReleasePublicationRequest &request,
                                                                const ReleasePublicationReceipt &receipt) {
        if (receipt.remoteIdentity.empty())
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        auto existing = ExistingRelease(client_, repository_, request, receipt.remoteIdentity);
        if (existing.HasError())
            return Result<void>::Failure(existing.ErrorValue());
        return client_.Commit(existing.Value(), request.channel, request.manifest.Digest());
    }
}  // namespace Horo::Release
