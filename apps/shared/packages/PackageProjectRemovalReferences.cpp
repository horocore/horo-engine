#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetRegistry.h"
#include "PackageProjectSession.h"

#include <algorithm>
#include <map>
#include <set>

namespace Horo::Application::Internal {
    namespace {
        constexpr std::size_t MaximumImporterDiagnostics = 256;

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
                if (importer.HasError()) {
                    if (failures_.size() < MaximumImporterDiagnostics)
                        failures_.emplace_back(DiagnosticCode{importer.ErrorValue().code.Value()}, DiagnosticSeverity::Error,
                                               importer.ErrorValue().message, SourceLocation{"packages.host"}, std::string{path});
                    else
                        truncated_ = true;
                    return bytes;
                }
                if (const auto match = removed_.find(importer.Value().importerPackageId); match != removed_.end()) {
                    const auto asset = path.substr(0, path.size() - std::string_view{".horo"}.size());
                    references_.emplace_back(*match->second, std::string{asset});
                }
                return bytes;
            }

            /** @brief Transfers deterministic accumulated observations after canonical inventory validation. */
            std::vector<PackageImporterReference> TakeReferences() {
                std::ranges::sort(references_, {}, &PackageImporterReference::assetPath);
                return std::move(references_);
            }

            /** @brief Adds importer failures only for metadata whose asset identity was independently admitted. */
            void AppendFailures(std::vector<Diagnostic> &diagnostics, const std::set<std::string_view> &admittedMetadata) const {
                for (const auto &failure : failures_)
                    if (admittedMetadata.contains(failure.path))
                        diagnostics.push_back(failure);
                if (truncated_)
                    diagnostics.emplace_back(DiagnosticCode{"packages.host.importer_reference_limit"}, DiagnosticSeverity::Error,
                                             "Additional importer metadata failures were omitted.", SourceLocation{"packages.host"}, "");
            }

            /** @brief Indicates whether the importer evidence needs to be reconciled with the asset inventory. */
            [[nodiscard]] bool HasFailures() const { return !failures_.empty(); }

        private:
            const Assets::IAssetRegistrySource &source_;
            std::map<std::string_view, const Packages::HoroPackageId *, std::less<>> removed_;
            mutable std::vector<PackageImporterReference> references_; /**< Confined to a single synchronous capture. */
            mutable std::vector<Diagnostic> failures_;
            mutable bool truncated_{};
        };

        /** @brief Reports importer and registry failures alongside claims from admitted assets. */
        Result<std::vector<PackageImporterReference>> ReviewImporterInventory(const Assets::AssetRegistryCandidate &candidate,
                                                                              ImporterEvidenceSource &capture) {
            using Report = Result<std::vector<PackageImporterReference>>;
            auto references = capture.TakeReferences();
            if (candidate.status == Assets::AssetRegistryBuildStatus::Complete && !capture.HasFailures())
                return Report::Success(std::move(references));
            auto error = MakeIncompletePackageInventoryError(candidate);
            std::set<std::string_view> admittedPaths;
            std::set<std::string_view> admittedMetadata;
            for (const auto &record : candidate.records) {
                admittedPaths.insert(record.sourcePath.String());
                admittedMetadata.insert(record.metadataPath.String());
            }
            capture.AppendFailures(error.diagnostics, admittedMetadata);
            if (error.diagnostics.empty() && candidate.status == Assets::AssetRegistryBuildStatus::Complete)
                return Report::Success(std::move(references));
            std::erase_if(references, [&admittedPaths](const auto &reference) {
                return !admittedPaths.contains(reference.assetPath);
            });
            AppendPackageImporterDiagnostics(error.diagnostics, references);
            return Report::Failure(std::move(error));
        }
    }  // namespace

    /** @copydoc MakeIncompletePackageInventoryError */
    Error MakeIncompletePackageInventoryError(const Assets::AssetRegistryCandidate &inventory) {
        Error error{ErrorCode{"packages.host.asset_inventory_incomplete"}, ErrorDomainId{"packages.host"}};
        if (!inventory.diagnostics.empty())
            error = WithCause(std::move(error), inventory.diagnostics.front().error);
        for (const auto &diagnostic : inventory.diagnostics)
            error.diagnostics.emplace_back(DiagnosticCode{diagnostic.error.code.Value()}, DiagnosticSeverity::Error,
                                           diagnostic.error.message, SourceLocation{"packages.host"}, diagnostic.projectPath.value_or(""));
        return error;
    }

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
        auto inventory = std::move(candidate).Value();
        auto report = ReviewImporterInventory(inventory, capture);
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
