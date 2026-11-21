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

        /** @brief Borrows the exact checked inventory and projects the root metadata for transport only. */
        template <typename Visitor> Result<void> VisitPublishedFiles(const ReleasePublicationRequest &request, Visitor visit) {
            for (const auto &artifact : request.manifest.Artifacts())
                if (auto accepted = visit(artifact); accepted.HasError())
                    return accepted;
            const ReleaseArtifactRecord manifest{std::string{ReleaseManifestFilename}, ReleaseArtifactRole::Notice,
                                                 request.manifest.CanonicalJson().size(), request.manifest.Digest()};
            if (auto accepted = visit(manifest); accepted.HasError())
                return accepted;
            const auto &signature = request.verified.ManifestSignature();
            if (!signature)
                return Result<void>::Success();
            const ReleaseArtifactRecord envelope{std::string{ReleaseManifestSignatureFilename}, ReleaseArtifactRole::Notice,
                                                 signature->size, signature->digest};
            return visit(envelope);
        }

        /** @brief Binds direct provider calls to the same frozen attempt as the publication dispatcher. */
        bool MatchesReceipt(const ReleasePublicationRequest &request, const ReleasePublicationReceipt &receipt) {
            return receipt.destination == ReleaseDestinationId{"github-releases"} && !receipt.remoteIdentity.empty() &&
                   receipt.candidate == request.verified.Candidate() && receipt.manifestDigest == request.verified.ManifestDigest() &&
                   receipt.artifactCount == request.manifest.Artifacts().size() &&
                   receipt.manifestSignature == request.verified.ManifestSignature();
        }
    }  // namespace

    /** @copydoc IGitHubReleaseClient::CommitVerified */
    Result<void> IGitHubReleaseClient::CommitVerified(const GitHubReleaseIdentity &release, const ReleaseChannel &channel,
                                                      const GitHubReleaseFinalMetadata &expected) {
        if (expected.manifestSignature)
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        const auto manifest = ReadAsset(release, ReleaseManifestFilename);
        if (manifest.HasError())
            return Result<void>::Failure(manifest.ErrorValue());
        if (manifest.Value().size != expected.manifestSize || manifest.Value().digest != expected.manifestDigest)
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        return Commit(release, channel, expected.manifestDigest);
    }

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
        const auto uploaded = VisitPublishedFiles(request, [&](const ReleaseArtifactRecord &record) {
            return client_.UploadExact(release, AssetName(record.path), request.verified.Root() / record.path, record);
        });
        if (uploaded.HasError())
            return Result<ReleasePublicationReceipt>::Failure(uploaded.ErrorValue());
        return Result<ReleasePublicationReceipt>::Success({Destination(), request.verified.Candidate(), request.verified.ManifestDigest(),
                                                           request.manifest.Artifacts().size(), std::to_string(release.releaseId),
                                                           request.verified.ManifestSignature()});
    }

    /** @copydoc GitHubReleasePublicationAdapter::VerifyRemote */
    Result<void> GitHubReleasePublicationAdapter::VerifyRemote(const ReleasePublicationRequest &request,
                                                               const ReleasePublicationReceipt &receipt) {
        if (!MatchesReceipt(request, receipt))
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        auto existing = ExistingRelease(client_, repository_, request, receipt.remoteIdentity);
        if (existing.HasError())
            return Result<void>::Failure(existing.ErrorValue());
        const auto &release = existing.Value();
        return VisitPublishedFiles(request, [&](const ReleaseArtifactRecord &record) {
            const auto name = AssetName(record.path);
            const auto remote = client_.ReadAsset(release, name);
            if (remote.HasError())
                return Result<void>::Failure(remote.ErrorValue());
            if (!MatchesEvidence(remote.Value(), name, record))
                return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
            return Result<void>::Success();
        });
    }

    /** @copydoc GitHubReleasePublicationAdapter::CommitChannel */
    Result<void> GitHubReleasePublicationAdapter::CommitChannel(const ReleasePublicationRequest &request,
                                                                const ReleasePublicationReceipt &receipt) {
        if (!MatchesReceipt(request, receipt))
            return Result<void>::Failure(MakeError(ReleaseErrors::PipelineOutputInvalid));
        auto existing = ExistingRelease(client_, repository_, request, receipt.remoteIdentity);
        if (existing.HasError())
            return Result<void>::Failure(existing.ErrorValue());
        const GitHubReleaseFinalMetadata expected{request.manifest.CanonicalJson().size(), request.verified.ManifestDigest(),
                                                  request.verified.ManifestSignature()};
        return client_.CommitVerified(existing.Value(), request.channel, expected);
    }
}  // namespace Horo::Release
