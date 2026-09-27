#pragma once

/**
 * @file GitHubReleaseCliClient.h
 * @brief GitHub Releases transport backed by the host's authenticated GitHub CLI.
 */

#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Release/GitHubReleasePublication.h"

namespace Horo::Release {
    /**
     * @brief Uses the host's `gh` authentication without putting credentials in arguments or diagnostics.
     *
     * The host owns the process runner and cancellation token. Calls are blocking and belong on a release worker.
     */
    class GitHubReleaseCliClient final : public IGitHubReleaseClient {
    public:
        /**
         * @brief Borrows a shell-free process runner.
         * @param processes Host-owned process capability.
         * @param cancellation Cancellation for each GitHub operation.
         */
        GitHubReleaseCliClient(IExternalProcessRunner &processes, CancellationToken cancellation = {}) noexcept;

        /** @copydoc IGitHubReleaseClient::FindExisting */
        [[nodiscard]] Result<GitHubReleaseIdentity> FindExisting(std::string_view repository, std::string_view tag) override;
        /** @copydoc IGitHubReleaseClient::UploadExact */
        [[nodiscard]] Result<void> UploadExact(const GitHubReleaseIdentity &release, std::string_view name,
                                               const std::filesystem::path &file, const ReleaseArtifactRecord &evidence) override;
        /** @copydoc IGitHubReleaseClient::ReadAsset */
        [[nodiscard]] Result<GitHubReleaseAssetEvidence> ReadAsset(const GitHubReleaseIdentity &release, std::string_view name) override;
        /** @copydoc IGitHubReleaseClient::Commit */
        [[nodiscard]] Result<void> Commit(const GitHubReleaseIdentity &release, const ReleaseChannel &channel,
                                          const Sha256Digest &manifestDigest) override;

    private:
        IExternalProcessRunner &processes_;
        CancellationToken cancellation_;

        [[nodiscard]] Result<std::string> Run(std::vector<std::string> arguments) const;
        [[nodiscard]] Result<std::string> ResolveSourceCommit(std::string_view repository, std::string_view tag) const;
        [[nodiscard]] Result<std::uint64_t> AssetId(const GitHubReleaseIdentity &release, std::string_view name) const;
        [[nodiscard]] Result<void> ConfirmIdentity(const GitHubReleaseIdentity &release);
    };
}  // namespace Horo::Release
