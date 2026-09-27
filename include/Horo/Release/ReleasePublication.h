#pragma once

/**
 * @file ReleasePublication.h
 * @brief Provider-neutral promotion of a final-verified immutable candidate.
 */

#include "Horo/Release/ReleaseCandidateVerification.h"

#include <cstdint>
#include <string>

namespace Horo::Release {
    /** @brief Built-in and project-defined destination channels. */
    enum class ReleaseChannelKind : std::uint8_t {
        Stable,
        Preview,
        Nightly,
        Enterprise,
        ProjectDefined
    };

    /** @brief One explicit publication channel; customId is set only for project-defined channels. */
    struct ReleaseChannel final {
        ReleaseChannelKind kind{ReleaseChannelKind::Stable};
        std::string customId;
    };

    /** @brief Immutable inputs for one publication attempt. */
    struct ReleasePublicationRequest final {
        const ReleaseExecutionPlan &plan;
        const VerifiedReleaseCandidate &verified;
        const ReleaseArtifactManifest &manifest;
        ReleaseChannel channel;
    };

    /** @brief Remote upload identity returned before a channel can be changed. */
    struct ReleasePublicationReceipt final {
        ReleaseDestinationId destination;
        ReleaseCandidateId candidate;
        Sha256Digest manifestDigest;
        std::uint64_t artifactCount{};
        std::string remoteIdentity; /**< Opaque provider ID bound to all later verification and commit calls. */
    };

    /** @brief Host-installed provider adapter with an explicit remote verification boundary. */
    class IReleasePublicationAdapter {
    public:
        virtual ~IReleasePublicationAdapter() = default;

        /** @brief Reports the single destination owned by this adapter. @return Destination ID. */
        [[nodiscard]] virtual ReleaseDestinationId Destination() const = 0;

        /**
         * @brief Uploads only the manifest-declared files without changing channel state.
         * @param request Final-verified candidate and selected channel.
         * @return Remote receipt or a typed failure; retries must use the same candidate identity.
         */
        [[nodiscard]] virtual Result<ReleasePublicationReceipt> Upload(const ReleasePublicationRequest &request) = 0;

        /**
         * @brief Verifies every uploaded remote artifact against the final manifest.
         * @param request Exact local candidate and manifest.
         * @param receipt Receipt returned by Upload.
         * @return Success only after remote byte and signature checks pass.
         */
        [[nodiscard]] virtual Result<void> VerifyRemote(const ReleasePublicationRequest &request,
                                                        const ReleasePublicationReceipt &receipt) = 0;

        /**
         * @brief Atomically changes the channel pointer after remote verification.
         * @param request Exact local candidate and selected channel.
         * @param receipt Verified remote receipt.
         * @return Success only when the channel resolves to this candidate.
         */
        [[nodiscard]] virtual Result<void> CommitChannel(const ReleasePublicationRequest &request,
                                                         const ReleasePublicationReceipt &receipt) = 0;
    };

    /**
     * @brief Publishes only an authorized, byte-stable, final-verified candidate.
     * @param request Frozen release plan, verified identity, final manifest, and channel.
     * @param adapter Explicit destination adapter; it owns provider-specific APIs and secrets.
     * @return Remote receipt only after upload, remote verification, and channel commit succeed.
     */
    [[nodiscard]] Result<ReleasePublicationReceipt> PublishVerifiedReleaseCandidate(const ReleasePublicationRequest &request,
                                                                                    IReleasePublicationAdapter &adapter);
}  // namespace Horo::Release
