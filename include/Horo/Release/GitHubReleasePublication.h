#pragma once

/**
 * @file GitHubReleasePublication.h
 * @brief GitHub-specific destination adapter for an existing tagged release.
 */

#include "Horo/Release/ReleasePublication.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Horo::Release {
    /** @brief Existing remote release selected by exact repository and source tag. */
    struct GitHubReleaseIdentity final {
        std::string repository;
        std::string tag;
        std::uint64_t releaseId{};
        std::string sourceCommit; /**< Peeled commit SHA of the existing Git tag. */
        std::string body;         /**< Reviewed release notes body returned by the existing GitHub Release. */
    };

    /** @brief Remote asset evidence measured by reading the uploaded bytes. */
    struct GitHubReleaseAssetEvidence final {
        std::string name;
        std::uint64_t size{};
        Sha256Digest digest;
    };

    /** @brief GitHub transport authority; credentials and HTTP details stay inside its implementation. */
    class IGitHubReleaseClient {
    public:
        virtual ~IGitHubReleaseClient() = default;

        /**
         * @brief Finds an existing authorized release without creating a tag, version, or release.
         * @param repository Canonical owner/repository identity.
         * @param tag Exact source tag for the candidate version.
         * @return Existing remote identity with the tag's peeled source commit, or a typed failure.
         */
        [[nodiscard]] virtual Result<GitHubReleaseIdentity> FindExisting(std::string_view repository, std::string_view tag) = 0;

        /**
         * @brief Uploads one exact file idempotently; a conflicting existing asset fails closed.
         * @param release Existing release identity.
         * @param name Reversible GitHub asset name for the manifest path.
         * @param file Local final-verified file.
         * @param evidence Expected exact size and digest.
         * @return Success only when the remote asset has this byte identity.
         */
        [[nodiscard]] virtual Result<void> UploadExact(const GitHubReleaseIdentity &release, std::string_view name,
                                                       const std::filesystem::path &file, const ReleaseArtifactRecord &evidence) = 0;

        /**
         * @brief Reads remote asset bytes and returns measured size and SHA-256 digest.
         * @param release Existing release identity.
         * @param name Reversible GitHub asset name.
         * @return Measured remote evidence or a typed failure.
         */
        [[nodiscard]] virtual Result<GitHubReleaseAssetEvidence> ReadAsset(const GitHubReleaseIdentity &release, std::string_view name) = 0;

        /**
         * @brief Commits the channel only after all remote assets were verified.
         * @param release Existing release identity.
         * @param channel Selected publication channel.
         * @param manifestDigest Exact finalized candidate metadata identity.
         * @return Success only when the channel points to this existing release idempotently.
         */
        [[nodiscard]] virtual Result<void> Commit(const GitHubReleaseIdentity &release, const ReleaseChannel &channel,
                                                  const Sha256Digest &manifestDigest) = 0;
    };

    /** @brief Provider adapter that never builds, repackages, or invents a source tag. */
    class GitHubReleasePublicationAdapter final : public IReleasePublicationAdapter {
    public:
        /** @brief Binds an existing repository and host-owned client. @param repository Canonical owner/name. @param client Client lifetime
         * owner. */
        GitHubReleasePublicationAdapter(std::string repository, IGitHubReleaseClient &client);

        /** @copydoc IReleasePublicationAdapter::Destination */
        [[nodiscard]] ReleaseDestinationId Destination() const override;
        /** @copydoc IReleasePublicationAdapter::Upload */
        [[nodiscard]] Result<ReleasePublicationReceipt> Upload(const ReleasePublicationRequest &request) override;
        /** @copydoc IReleasePublicationAdapter::VerifyRemote */
        [[nodiscard]] Result<void> VerifyRemote(const ReleasePublicationRequest &request,
                                                const ReleasePublicationReceipt &receipt) override;
        /** @copydoc IReleasePublicationAdapter::CommitChannel */
        [[nodiscard]] Result<void> CommitChannel(const ReleasePublicationRequest &request,
                                                 const ReleasePublicationReceipt &receipt) override;

    private:
        std::string repository_;
        IGitHubReleaseClient &client_;
    };
}  // namespace Horo::Release
