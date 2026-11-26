#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetRegistry.h"
#include "PackageProjectSession.h"

#include <algorithm>
#include <map>
#include <set>

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

        /** @brief Reports inventory failures alongside importer claims from records admitted by the registry. */
        Error IncompleteImporterInventory(const Assets::AssetRegistryCandidate &candidate,
                                          std::vector<PackageImporterReference> references) {
            Error error{ErrorCode{"packages.host.asset_inventory_incomplete"}, ErrorDomainId{"packages.host"}};
            if (!candidate.diagnostics.empty())
                error = WithCause(std::move(error), candidate.diagnostics.front().error);
            for (const auto &diagnostic : candidate.diagnostics)
                error.diagnostics.emplace_back(DiagnosticCode{diagnostic.error.code.Value()}, DiagnosticSeverity::Error,
                                               diagnostic.error.message, SourceLocation{"packages.host"},
                                               diagnostic.projectPath.value_or(""));
            std::set<std::string_view> admittedPaths;
            for (const auto &record : candidate.records)
                admittedPaths.insert(record.sourcePath.String());
            std::erase_if(references, [&admittedPaths](const auto &reference) {
                return !admittedPaths.contains(reference.assetPath);
            });
            AppendPackageImporterDiagnostics(error.diagnostics, references);
            return error;
        }
    }  // namespace

    /** @copydoc InspectPackageRemovalImporters */
    Result<std::vector<PackageImporterReference>> InspectPackageRemovalImporters(const Assets::IAssetRegistrySource &assets,
                                                                                 const Packages::PackageRemovalGraphPreview &removal,
                                                                                 Assets::AssetRegistryCandidate *reviewed) {
        using Report = Result<std::vector<PackageImporterReference>>;
        if (removal.removed.empty())
            return Report::Success({});
        ImporterEvidenceSource capture{assets, removal.removed};
        auto candidate = Assets::PrepareAssetRegistryCandidate(capture);
        if (candidate.HasError())
            return Report::Failure(candidate.ErrorValue());
        auto references = std::move(capture).TakeReferences();
        auto inventory = std::move(candidate).Value();
        auto report = inventory.status == Assets::AssetRegistryBuildStatus::Complete
                          ? Report::Success(std::move(references))
                          : Report::Failure(IncompleteImporterInventory(inventory, std::move(references)));
        if (reviewed)
            *reviewed = std::move(inventory);
        return report;
    }

    /** @copydoc AppendPackageImporterDiagnostics */
    void AppendPackageImporterDiagnostics(std::vector<Diagnostic> &diagnostics, const std::span<const PackageImporterReference> references) {
        for (const auto &reference : references)
            diagnostics.emplace_back(DiagnosticCode{"packages.host.importer_in_use"}, DiagnosticSeverity::Error,
                                     reference.package.Value(), SourceLocation{"packages.host"}, reference.assetPath);
    }
}  // namespace Horo::Application::Internal
