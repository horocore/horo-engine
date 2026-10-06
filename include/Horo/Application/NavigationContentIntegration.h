#pragma once

/** @file NavigationContentIntegration.h
 * @brief Explicit release composition of promoted navigation content without recooking or backend discovery. */

#include "Horo/Assets/AssetArchive.h"
#include "Horo/Navigation/NavMeshAssetLoading.h"
#include "Horo/Release/ReleaseArtifactManifest.h"
#include "Horo/Release/ReleaseCandidateVerification.h"

namespace Horo::Application {
    /** @brief Complete bounded archive and inert manifest evidence, prepared before any output publication.
     * @details The host stages these exact archive bytes and includes the extension in its final manifest under its signing policy.
     * This value grants no package trust and invokes no signing, native provider, builder or filesystem writer. */
    struct PreparedNavigationReleaseContent final {
        std::vector<std::uint8_t> archive;
        Release::ReleaseManifestExtension extension;
        std::vector<Navigation::NavMeshAssetContentExpectation> expectations;
    };

    /** @brief Validate pinned immutable envelopes and complete HNS2 project policy before archive construction.
     * @param generation Exact promoted generation; mutable current.json is never consulted.
     * @param plan Existing validated chunk authority, including actual Base/DedicatedServer roles.
     * @param product Explicit GameRuntime or GameDedicatedServer role.
     * @param limits Existing aggregate/archive bounds; encoded content is hashed and decoded only on this control path.
     * @return Complete archive and canonical namespaced evidence, or original typed failure before any output exists.
     * @note Every selected NavMesh must carry an owned validated project profile. HNS1 and omitted policy fail closed. */
    [[nodiscard]] Result<PreparedNavigationReleaseContent> PrepareNavigationReleaseContent(const Assets::AssetCookGeneration &generation,
                                                                                           const Assets::AssetChunkPlan &plan,
                                                                                           Release::DistributionProductKind product,
                                                                                           const Assets::AssetArchiveLimits &limits = {});

    /** @brief Immutable provider and owned exact expectations admitted by the host release verification policy. */
    struct AdmittedNavigationReleaseContent final {
        Assets::AssetArchiveProvider provider;
        std::vector<Navigation::NavMeshAssetContentExpectation> expectations;
    };

    /** @brief Bind exact archive bytes to verified inventory and the namespaced navigation closure before runtime exposure.
     * @param archive Complete archive bytes supplied by the verified package host.
     * @param manifest Exact canonical final manifest admitted by the existing release verifier.
     * @param verified Construction-guarded candidate proof, retained by the caller through this call.
     * The proof must match both candidate identity and manifest digest. Policy-permitted unsigned candidates are
     * policy-admitted; this function does not describe them as authenticated signed content.
     * @param archivePath Exact declared AssetArchive path, never inferred from a basename.
     * @param target Explicit runtime cook target, independent of renderer selection.
     * @param product Exact runtime or dedicated-server product selected by the host.
     * @param limits Finite archive, artifact and inventory bounds.
     * @return Complete provider plus exact per-asset policy, or typed failure before any provider escapes.
     * @note Canonical parsing and digest equality are integrity checks, not signature verification or trust-root selection. */
    [[nodiscard]] Result<AdmittedNavigationReleaseContent> AdmitNavigationReleaseContent(
        std::span<const std::uint8_t> archive, const Release::ReleaseArtifactManifest &manifest,
        const Release::VerifiedReleaseCandidate &verified, std::string_view archivePath, const AssetCookTargetId &target,
        Release::DistributionProductKind product, const Assets::AssetArchiveLimits &limits = {});

    /** @brief Existing RuntimeAssets smoke seam validates private candidate content without exposing a runtime provider.
     * @details The generic verifier performs inventory/signature checks and issues the candidate proof only after
     * this probe succeeds. Selection is explicit; no provider, signer, trust root or runtime host is discovered. */
    class NavigationReleaseContentSmokeProbe final : public Release::IReleaseCandidateSmokeProbe {
    public:
        /** @brief Bind exact content path and application-owned target/product policy.
         * @param archivePath Portable declared AssetArchive path.
         * @param target Explicit cook target. @param product Game runtime or dedicated-server product.
         * @param limits Finite artifact/archive bounds. */
        NavigationReleaseContentSmokeProbe(std::string archivePath, AssetCookTargetId target, Release::DistributionProductKind product,
                                           Assets::AssetArchiveLimits limits = {});
        /** @copydoc Release::IReleaseCandidateSmokeProbe::Kind */
        [[nodiscard]] Release::ReleaseCandidateSmokeKind Kind() const noexcept override;
        /** @copydoc Release::IReleaseCandidateSmokeProbe::Check */
        [[nodiscard]] Result<void> Check(const std::filesystem::path &root, const Release::ReleaseArtifactManifest &manifest) override;

    private:
        std::string archivePath_;
        AssetCookTargetId target_;
        Release::DistributionProductKind product_;
        Assets::AssetArchiveLimits limits_;
    };
}  // namespace Horo::Application
