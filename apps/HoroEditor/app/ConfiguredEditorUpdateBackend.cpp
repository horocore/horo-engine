#include "ConfiguredEditorUpdateBackend.h"

#include "Horo/Release/UpdateManifestErrors.h"
#include "Horo/Release/UpdateTransferErrors.h"

#include <utility>
#include <variant>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] std::string VersionText(const Release::ReleaseProductVersion &version) {
            return std::visit([](const auto &value) {
                return Release::FormatReleaseVersion(value.value);
            }, version);
        }

        [[nodiscard]] Result<void> InvalidOffer() {
            return Result<void>::Failure(MakeError(Release::UpdateManifestErrors::Incompatible));
        }
    }  // namespace

    ZipEditorUpdatePackageStager::ZipEditorUpdatePackageStager(EditorUpdateStagerPolicy policy, NativeDurableFileSystem &files)
        : policy_(std::move(policy)), files_(files) {}

    Result<std::filesystem::path> ZipEditorUpdatePackageStager::Prepare(const Release::UpdatePackageRecord &package,
                                                                        const Security::ArtifactVerifier &verifier,
                                                                        const CancellationToken cancellation,
                                                                        const Release::UpdateDownloadProgress &progress) {
        if (package.selection.format != Release::DistributionPackageFormat::ZipArchive)
            return Result<std::filesystem::path>::Failure(MakeError(Release::UpdateTransferErrors::StageMismatch));
        return Release::PrepareZipUpdateStageHttps({package, policy_.downloadPaths, policy_.stageRoot, policy_.downloadLimits,
                                                    policy_.archiveLimits, policy_.httpPolicy},
                                                   files_, verifier, cancellation, progress);
    }

    TarGzipEditorUpdatePackageStager::TarGzipEditorUpdatePackageStager(EditorUpdateStagerPolicy policy, NativeDurableFileSystem &files)
        : policy_(std::move(policy)), files_(files) {}

    Result<std::filesystem::path> TarGzipEditorUpdatePackageStager::Prepare(const Release::UpdatePackageRecord &package,
                                                                            const Security::ArtifactVerifier &verifier,
                                                                            const CancellationToken cancellation,
                                                                            const Release::UpdateDownloadProgress &progress) {
        if (package.selection.format != Release::DistributionPackageFormat::TarGzip)
            return Result<std::filesystem::path>::Failure(MakeError(Release::UpdateTransferErrors::StageMismatch));
        return Release::PrepareTarGzipUpdateStageHttps({package, policy_.downloadPaths, policy_.stageRoot, policy_.downloadLimits,
                                                        policy_.archiveLimits, policy_.httpPolicy},
                                                       files_, verifier, cancellation, progress);
    }

    ConfiguredEditorUpdateBackend::ConfiguredEditorUpdateBackend(ConfiguredEditorUpdatePolicy policy, IEditorUpdateManifestSource &source,
                                                                 IEditorUpdatePackageStager &stager, IEditorUpdateHandoff &handoff)
        : policy_(std::move(policy)), source_(source), stager_(stager), handoff_(handoff) {}

    Result<std::optional<EditorUpdateOffer>> ConfiguredEditorUpdateBackend::Check(const EditorUpdateChannel &channel,
                                                                                  const CancellationToken cancellation) {
        {
            std::lock_guard lock(mutex_);
            selectedPackage_.reset();
            readyMarker_.clear();
            ++selectedOfferId_;
        }
        auto fetched = source_.Fetch(channel, cancellation);
        if (fetched.HasError())
            return Result<std::optional<EditorUpdateOffer>>::Failure(fetched.ErrorValue());
        if (fetched.Value().expectedChannel.empty() || !policy_.trustedUnixTime)
            return Result<std::optional<EditorUpdateOffer>>::Failure(MakeError(Release::UpdateManifestErrors::Incompatible));
        auto parsed = Release::SignedUpdateManifest::ParseCanonical(fetched.Value().signedDocument);
        if (parsed.HasError())
            return Result<std::optional<EditorUpdateOffer>>::Failure(parsed.ErrorValue());

        auto admission = policy_.admission;
        admission.channel = fetched.Value().expectedChannel;
        admission.now = policy_.trustedUnixTime();
        const auto assessed =
            Release::AssessUpdate(parsed.Value(), admission, policy_.roots, policy_.signatureProvider, policy_.preferences);
        if (assessed.status == Release::UpdateDiscoveryStatus::UpToDate) {
            return Result<std::optional<EditorUpdateOffer>>::Success(std::nullopt);
        }
        if (assessed.status != Release::UpdateDiscoveryStatus::Available || !assessed.package)
            return Result<std::optional<EditorUpdateOffer>>::Failure(
                assessed.failure.value_or(MakeError(Release::UpdateManifestErrors::Incompatible)));

        EditorUpdateOffer offer;
        offer.version = VersionText(parsed.Value().Data().version);
        offer.releaseNotes = parsed.Value().Data().releaseNotes;
        offer.compatibilityImpacts = parsed.Value().Data().compatibilityImpacts;
        offer.requiresRestart = true;
        {
            std::lock_guard lock(mutex_);
            selectedPackage_ = *assessed.package;
            readyMarker_.clear();
            offer.id = ++selectedOfferId_;
        }
        return Result<std::optional<EditorUpdateOffer>>::Success(std::move(offer));
    }

    Result<void> ConfiguredEditorUpdateBackend::Prepare(
        const EditorUpdateOffer &offer, const CancellationToken cancellation,
        const std::function<void(EditorUpdatePhase, std::uint64_t, std::uint64_t)> &progress) {
        Release::UpdatePackageRecord package;
        {
            std::lock_guard lock(mutex_);
            if (!selectedPackage_ || offer.id == 0U || offer.id != selectedOfferId_)
                return InvalidOffer();
            package = *selectedPackage_;
        }
        Security::ArtifactVerifier verifier{policy_.signatureProvider, policy_.roots.Roots()};
        auto staged =
            stager_.Prepare(package, verifier, cancellation, [&progress](const std::uint64_t transferred, const std::uint64_t total) {
            if (progress)
                progress(EditorUpdatePhase::Downloading, transferred, total);
        });
        if (staged.HasError())
            return Result<void>::Failure(staged.ErrorValue());
        if (progress)
            progress(EditorUpdatePhase::Verifying, package.size, package.size);
        return CommitReadyMarker(offer.id, std::move(staged).Value());
    }

    Result<void> ConfiguredEditorUpdateBackend::CommitReadyMarker(const std::uint64_t offerId, std::filesystem::path marker) {
        std::lock_guard lock(mutex_);
        if (!selectedPackage_ || offerId != selectedOfferId_)
            return InvalidOffer();
        readyMarker_ = std::move(marker);
        return Result<void>::Success();
    }

    Result<void> ConfiguredEditorUpdateBackend::Activate(const CancellationToken cancellation) {
        Release::UpdatePackageRecord package;
        std::filesystem::path marker;
        {
            std::lock_guard lock(mutex_);
            if (!selectedPackage_ || readyMarker_.empty())
                return Result<void>::Failure(MakeError(Release::UpdateTransferErrors::StageMismatch));
            package = *selectedPackage_;
            marker = readyMarker_;
        }
        return handoff_.RequestActivation(package, marker, cancellation);
    }

    Result<void> ConfiguredEditorUpdateBackend::Rollback(const CancellationToken cancellation) {
        return handoff_.RequestRollback(cancellation);
    }
}  // namespace Horo::Editor
