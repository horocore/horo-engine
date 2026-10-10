#include "PackageActivationComposition.h"

#include "Horo/Packages/PackageLifecycleErrors.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <stdexcept>

namespace Horo::Packages::Detail {
    namespace {
        /** @brief Binds the existing native security gate to the exact artifact selected from this immutable archive. */
        class BoundArtifactGate final : public Security::NativeArtifactGate {
        public:
            BoundArtifactGate(std::shared_ptr<const Security::NativeArtifactGate> gate,
                              std::map<std::filesystem::path, Sha256Digest> artifacts)
                : gate_(std::move(gate)), artifacts_(std::move(artifacts)) {}

            Result<Security::VerifiedArtifactEvidence> Verify(const std::filesystem::path &path) const override {
                const auto expected = artifacts_.find(path.lexically_normal());
                if (expected == artifacts_.end())
                    return Result<Security::VerifiedArtifactEvidence>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
                auto evidence = gate_->Verify(path);
                if (evidence.HasValue() && evidence.Value().ArtifactDigest() != expected->second)
                    return Result<Security::VerifiedArtifactEvidence>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
                return evidence;
            }

        private:
            std::shared_ptr<const Security::NativeArtifactGate> gate_;
            std::map<std::filesystem::path, Sha256Digest> artifacts_;
        };

        /** @brief Exclusively creates one bounded private directory; existing names and symlinks are never adopted. */
        Result<std::filesystem::path> CreatePrivateRoot(const std::filesystem::path &parent) {
            static std::atomic<std::uint64_t> sequence{};  // Only unique temporary names are shared across service owners.
            for (unsigned attempt = 0; attempt < 16U; ++attempt) {
                auto path = parent / ("horo-activation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                                      '-' + std::to_string(++sequence));
                std::error_code error;
                if (!std::filesystem::create_directory(path, error)) {
                    if (error)
                        return Result<std::filesystem::path>::Failure(MakeError(PackageLifecycleErrors::StorageFailed));
                    continue;
                }
                std::filesystem::permissions(path, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, error);
                if (error) {
                    std::filesystem::remove(path, error);
                    return Result<std::filesystem::path>::Failure(MakeError(PackageLifecycleErrors::StorageFailed));
                }
                return Result<std::filesystem::path>::Success(std::move(path));
            }
            return Result<std::filesystem::path>::Failure(MakeError(PackageLifecycleErrors::StorageFailed));
        }
    }  // namespace

    /** @copydoc PackageActivationComposition::PackageActivationComposition */
    PackageActivationComposition::PackageActivationComposition(const PackageLifecycleConfiguration &configuration,
                                                               std::shared_ptr<const VerifiedPackageInstallRecord> install,
                                                               const std::uint64_t generation)
        : snapshot_(std::make_shared<PackageActivationSnapshot>()), catalog_(std::make_unique<Assets::AssetImporterCatalog>()) {
        (void)configuration;
        snapshot_->install = std::move(install);
        snapshot_->generation = generation;
    }

    /** @copydoc PackageActivationComposition::~PackageActivationComposition */
    PackageActivationComposition::~PackageActivationComposition() noexcept {
        Retire();
        // Held callback/catalog leases may still read package resources. Preserve verified files for restart recovery.
        removeContent_ = std::ranges::all_of(retirements_, [](const auto &retirement) {
            return retirement->IsDrained();
        });
        if (manager_)
            removeContent_ = removeContent_ && std::ranges::all_of(manager_->FailedActivationRetirements(), [](const auto &retirement) {
                return retirement->IsDrained();
            });
        manager_.reset();
        catalog_.reset();
        if (removeContent_ && !root_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }
    }

    /** @copydoc PackageActivationComposition::Materialize */
    Result<void> PackageActivationComposition::Materialize(const PackageActivationCandidate &candidate, const std::filesystem::path &root,
                                                           const CancellationToken &cancellation) const {
        const auto &package = candidate.install->Graph()->packages[candidate.packageIndex];
        const auto descriptorRoot = candidate.descriptor.Value().substr(0, candidate.descriptor.Value().rfind('/'));
        for (const auto &entry : package.archive->Manifest().Entries()) {
            if (!entry.id.path.Value().starts_with(descriptorRoot + '/'))
                continue;
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::Cancelled));
            auto bytes = package.archive->ReadDeclaredFile(entry.id.path, entry.size);
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            const auto path = root / std::filesystem::path{entry.id.path.Value()};
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::StorageFailed));
            std::ofstream output{path, std::ios::binary | std::ios::trunc};
            const auto &content = bytes.Value();
            output.write(reinterpret_cast<const char *>(content.data()), static_cast<std::streamsize>(content.size()));
            output.close();
            if (!output)
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::StorageFailed));
            const auto permissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                     (entry.executable ? std::filesystem::perms::owner_exec : std::filesystem::perms::none);
            std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, error);
            if (error)
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::StorageFailed));
        }
        return Result<void>::Success();
    }

    /** @copydoc PackageActivationComposition::Prepare */
    Result<void> PackageActivationComposition::Prepare(const std::span<const PackageActivationCandidate> candidates,
                                                       const PackageLifecycleConfiguration &configuration,
                                                       const CancellationToken &cancellation) {
        auto root = CreatePrivateRoot(configuration.temporaryRoot);
        if (root.HasError())
            return Result<void>::Failure(root.ErrorValue());
        root_ = std::move(root).Value();
        std::map<std::filesystem::path, Sha256Digest> artifacts;
        // Evaluate every grant before any package code runs. Leases stay detached until complete registration succeeds.
        for (const auto &candidate : candidates) {
            for (const auto &id : candidate.plan.moduleIds) {
                const auto module = std::ranges::find(candidate.manifest.modules, id, &Extensions::ExtensionModuleManifest::id);
                Extensions::ExtensionAdmissionRequest request{candidate.manifest.id, id, snapshot_->generation, {}};
                for (const auto &capability : module->requiredCapabilities)
                    request.capabilities.push_back({{capability}, {}});
                auto admission = Extensions::ExtensionCapabilityAdmission::Evaluate(request, candidate.trust.policy);
                if (admission.HasError())
                    return Result<void>::Failure(admission.ErrorValue());
                snapshot_->activations.push_back(admission.Value().ActivationLease());
                admissions_.push_back(std::move(admission).Value());
            }
            const auto packageRoot = root_ / std::to_string(candidate.packageIndex);
            if (auto files = Materialize(candidate, packageRoot, cancellation); files.HasError())
                return files;
            const auto &archive = candidate.install->Graph()->packages[candidate.packageIndex].archive;
            for (const auto &file : archive->Manifest().Entries())
                artifacts.emplace(packageRoot / file.id.path.Value(), file.digest);
        }
        auto gate = std::make_shared<BoundArtifactGate>(configuration.artifactGate, std::move(artifacts));
        manager_ = std::make_unique<Extensions::ExtensionManager>(catalog_.get(), configuration.profile, configuration.capabilities,
                                                                  std::move(gate), configuration.libraryLoader);
        retirements_.reserve(candidates.size());
        for (const auto &candidate : candidates) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PackageLifecycleErrors::Cancelled));
            const auto packageRoot = root_ / std::to_string(candidate.packageIndex);
            const auto descriptor = packageRoot / candidate.descriptor.Value();
            auto loaded = manager_->LoadExtension(descriptor.parent_path().string(), candidate.providers);
            if (loaded.HasError())
                return Result<void>::Failure(loaded.ErrorValue());
            retirements_.push_back(manager_->Retirement(loaded.Value()));
        }
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(PackageLifecycleErrors::Cancelled));
        auto publication = catalog_->Publish();
        if (publication.HasError())
            return Result<void>::Failure(publication.ErrorValue());
        snapshot_->importers = std::move(publication).Value();
        return Result<void>::Success();
    }

    /** @copydoc PackageActivationComposition::Retire */
    void PackageActivationComposition::Retire() noexcept {
        if (retired_)
            return;
        retired_ = true;
        for (auto admission = admissions_.rbegin(); admission != admissions_.rend(); ++admission)
            admission->Revoke();
        snapshot_.reset();
        if (manager_) {
            try {
                manager_->UnloadAll();
            } catch (...) {
                for (const auto &retirement : retirements_) {
                    retirement->CloseAdmission();
                    retirement->RequireRestart();
                }
            }
        }
    }

    /** @copydoc PackageActivationComposition::Finalize */
    bool PackageActivationComposition::Finalize() {
        if (!retired_)
            return false;
        if (manager_)
            manager_->FinalizeRetirements();
        return std::ranges::all_of(retirements_, [](const auto &retirement) {
            return retirement->IsDrained();
        }) && (!manager_ || std::ranges::all_of(manager_->FailedActivationRetirements(), [](const auto &retirement) {
            return retirement->IsDrained();
        }));
    }

    /** @copydoc PackageActivationComposition::Snapshot */
    std::shared_ptr<const PackageActivationSnapshot> PackageActivationComposition::Snapshot() const noexcept {
        return snapshot_;
    }
}  // namespace Horo::Packages::Detail
