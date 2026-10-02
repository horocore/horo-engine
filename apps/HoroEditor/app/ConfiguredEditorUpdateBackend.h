#pragma once

#include "Horo/Release/UpdateDiscovery.h"
#include "Horo/Release/UpdateTarGzipStagingJob.h"
#include "Horo/Release/UpdateZipStagingJob.h"
#include "editor/update/UpdateExperienceSession.h"

#include <functional>
#include <mutex>
#include <optional>

namespace Horo::Editor {
    /** @brief Canonical bytes from a host-selected source and its configured channel identity. */
    struct EditorUpdateMetadata final {
        std::string signedDocument;
        std::string expectedChannel;
    };

    /** @brief Host-owned source selection; fetched bytes never select their own trusted channel. */
    class IEditorUpdateManifestSource {
    public:
        virtual ~IEditorUpdateManifestSource() = default;
        [[nodiscard]] virtual Result<EditorUpdateMetadata> Fetch(const EditorUpdateChannel &channel, CancellationToken cancellation) = 0;
    };

    /** @brief Durable request to a separate updater helper, made without switching the running installation. */
    class IEditorUpdateHandoff {
    public:
        virtual ~IEditorUpdateHandoff() = default;
        [[nodiscard]] virtual Result<void> RequestActivation(const Release::UpdatePackageRecord &package,
                                                             const std::filesystem::path &readyMarker, CancellationToken cancellation) = 0;
        [[nodiscard]] virtual Result<void> RequestRollback(CancellationToken cancellation) = 0;
    };

    /** @brief Format-specific verified package staging selected by the editor host. */
    class IEditorUpdatePackageStager {
    public:
        virtual ~IEditorUpdatePackageStager() = default;
        [[nodiscard]] virtual Result<std::filesystem::path> Prepare(const Release::UpdatePackageRecord &package,
                                                                    const Security::ArtifactVerifier &verifier,
                                                                    CancellationToken cancellation,
                                                                    const Release::UpdateDownloadProgress &progress) = 0;
    };

    /** @brief Private archive staging paths and limits selected by the installed-product host. */
    struct EditorUpdateStagerPolicy final {
        Release::UpdateDownloadPaths downloadPaths;
        std::filesystem::path stageRoot;
        Release::UpdateDownloadLimits downloadLimits;
        Release::UpdateArchiveLimits archiveLimits;
        Release::UpdateHttpDownloadPolicy httpPolicy;
    };

    /** @brief Windows/macOS portable ZIP implementation of the format-specific staging boundary. */
    class ZipEditorUpdatePackageStager final : public IEditorUpdatePackageStager {
    public:
        ZipEditorUpdatePackageStager(EditorUpdateStagerPolicy policy, NativeDurableFileSystem &files);
        [[nodiscard]] Result<std::filesystem::path> Prepare(const Release::UpdatePackageRecord &package,
                                                            const Security::ArtifactVerifier &verifier, CancellationToken cancellation,
                                                            const Release::UpdateDownloadProgress &progress) override;

    private:
        EditorUpdateStagerPolicy policy_;
        NativeDurableFileSystem &files_;
    };

    /** @brief Linux portable tar.gz implementation of the format-specific staging boundary. */
    class TarGzipEditorUpdatePackageStager final : public IEditorUpdatePackageStager {
    public:
        TarGzipEditorUpdatePackageStager(EditorUpdateStagerPolicy policy, NativeDurableFileSystem &files);
        [[nodiscard]] Result<std::filesystem::path> Prepare(const Release::UpdatePackageRecord &package,
                                                            const Security::ArtifactVerifier &verifier, CancellationToken cancellation,
                                                            const Release::UpdateDownloadProgress &progress) override;

    private:
        EditorUpdateStagerPolicy policy_;
        NativeDurableFileSystem &files_;
    };

    /** @brief Installer-authenticated update facts and private host paths, never loaded from untrusted source bytes. */
    struct ConfiguredEditorUpdatePolicy final {
        Release::UpdateAdmissionContext admission;
        Release::UpdateTrustRootSnapshot roots;
        std::shared_ptr<const Security::SignatureProvider> signatureProvider;
        Release::UpdatePackagePreferences preferences;
        std::function<std::uint64_t()> trustedUnixTime;
    };

    /** @brief Worker adapter from verified discovery to private staging and updater-helper handoff. */
    class ConfiguredEditorUpdateBackend final : public IEditorUpdateBackend {
    public:
        ConfiguredEditorUpdateBackend(ConfiguredEditorUpdatePolicy policy, IEditorUpdateManifestSource &source,
                                      IEditorUpdatePackageStager &stager, IEditorUpdateHandoff &handoff);

        [[nodiscard]] Result<std::optional<EditorUpdateOffer>> Check(const EditorUpdateChannel &channel,
                                                                     CancellationToken cancellation) override;
        [[nodiscard]] Result<void> Prepare(const EditorUpdateOffer &offer, CancellationToken cancellation,
                                           const std::function<void(EditorUpdatePhase, std::uint64_t, std::uint64_t)> &progress) override;
        [[nodiscard]] Result<void> Activate(CancellationToken cancellation) override;
        [[nodiscard]] Result<void> Rollback(CancellationToken cancellation) override;

    private:
        [[nodiscard]] Result<void> CommitReadyMarker(std::uint64_t offerId, std::filesystem::path marker);

        ConfiguredEditorUpdatePolicy policy_;
        IEditorUpdateManifestSource &source_;
        IEditorUpdatePackageStager &stager_;
        IEditorUpdateHandoff &handoff_;
        std::mutex mutex_;
        std::optional<Release::UpdatePackageRecord> selectedPackage_;
        std::filesystem::path readyMarker_;
        std::uint64_t selectedOfferId_{};
    };
}  // namespace Horo::Editor
