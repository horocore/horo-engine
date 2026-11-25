#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetRegistry.h"
#include "PackageProjectSession.h"

#include <algorithm>
#include <map>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Collects importer evidence from the same bytes as the canonical registry candidate. */
        class ImporterEvidenceSource final : public Assets::IAssetRegistrySource {
        public:
            ImporterEvidenceSource(const Assets::IAssetRegistrySource &source, std::span<const Packages::LockedPackageReference> removed)
                : source_(source) {
                for (const auto &package : removed)
                    removed_.try_emplace(package.package.Value(), &package.package);
            }

            /** @copydoc Assets::IAssetRegistrySource::Capture */
            Result<Assets::AssetRegistrySourceManifest> Capture() const override {
                return source_.Capture();
            }

            /** @copydoc Assets::IAssetRegistrySource::ReadSidecar */
            Result<std::string> ReadSidecar(std::string_view path, std::chrono::steady_clock::time_point deadline) const override {
                auto bytes = source_.ReadSidecar(path, deadline);
                if (bytes.HasError())
                    return bytes;
                const auto importer = Assets::DecodeAssetImportMetadata(bytes.Value());
                if (importer.HasError())
                    return Result<std::string>::Failure(importer.ErrorValue());
                if (const auto match = removed_.find(importer.Value().importerPackageId); match != removed_.end()) {
                    const auto asset = path.substr(0, path.size() - std::string_view{".horo"}.size());
                    references_.emplace_back(*match->second, std::string{asset});
                }
                return bytes;
            }

            /** @brief Transfers deterministic accumulated observations after canonical inventory validation. */
            std::vector<PackageImporterReference> TakeReferences() && {
                std::ranges::sort(references_, {}, &PackageImporterReference::assetPath);
                return std::move(references_);
            }

        private:
            const Assets::IAssetRegistrySource &source_;
            std::map<std::string_view, const Packages::HoroPackageId *, std::less<>> removed_;
            mutable std::vector<PackageImporterReference> references_; /**< Confined to a single synchronous capture. */
        };
    }  // namespace

    /** @copydoc InspectPackageRemovalImporters */
    Result<std::vector<PackageImporterReference>> InspectPackageRemovalImporters(const Assets::IAssetRegistrySource &assets,
                                                                                 const Packages::PackageRemovalGraphPreview &removal) {
        using Report = Result<std::vector<PackageImporterReference>>;
        if (removal.removed.empty())
            return Report::Success({});
        ImporterEvidenceSource capture{assets, removal.removed};
        const auto candidate = Assets::PrepareAssetRegistryCandidate(capture);
        if (candidate.HasError())
            return Report::Failure(candidate.ErrorValue());
        if (candidate.Value().status != Assets::AssetRegistryBuildStatus::Complete) {
            Error error{ErrorCode{"packages.host.asset_inventory_incomplete"}, ErrorDomainId{"packages.host"}};
            if (!candidate.Value().diagnostics.empty())
                error = WithCause(std::move(error), candidate.Value().diagnostics.front().error);
            return Report::Failure(std::move(error));
        }
        return Report::Success(std::move(capture).TakeReferences());
    }

    /** @copydoc AppendPackageImporterDiagnostics */
    void AppendPackageImporterDiagnostics(std::vector<Diagnostic> &diagnostics, const std::span<const PackageImporterReference> references) {
        for (const auto &reference : references)
            diagnostics.emplace_back(DiagnosticCode{"packages.host.importer_in_use"}, DiagnosticSeverity::Error,
                                     reference.package.Value(), SourceLocation{"packages.host"}, reference.assetPath);
    }
}  // namespace Horo::Application::Internal
