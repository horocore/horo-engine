#include "Horo/Packages/PackageLifecycleErrors.h"
#include "PackageModuleCandidateInternal.h"

#include <algorithm>

namespace Horo::Packages {
    /** @copydoc VerifiedPackageModule::VerifiedPackageModule */
    VerifiedPackageModule::VerifiedPackageModule(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc VerifiedPackageModule::Install */
    const std::shared_ptr<const VerifiedPackageInstallRecord> &VerifiedPackageModule::Install() const noexcept {
        return state_->install;
    }

    /** @copydoc VerifiedPackageModule::Package */
    const PackageRestorePackage &VerifiedPackageModule::Package() const noexcept {
        return state_->install->Graph()->packages[state_->packageIndex];
    }

    /** @copydoc VerifiedPackageModule::Manifest */
    const Extensions::ExtensionManifest &VerifiedPackageModule::Manifest() const noexcept {
        return *state_->manifest;
    }

    /** @copydoc VerifiedPackageModule::Module */
    const Extensions::ExtensionModuleManifest &VerifiedPackageModule::Module() const noexcept {
        return state_->manifest->modules[state_->moduleIndex];
    }

    /** @copydoc VerifiedPackageModule::Identity */
    const PackageModuleReport &VerifiedPackageModule::Identity() const noexcept {
        return state_->identity;
    }

    /** @copydoc VerifiedPackageModule::Descriptor */
    const PackagePath &VerifiedPackageModule::Descriptor() const noexcept {
        return state_->descriptor;
    }

    /** @copydoc VerifiedPackageModule::Artifact */
    const PackagePath &VerifiedPackageModule::Artifact() const noexcept {
        return state_->artifact;
    }

    /** @copydoc VerifiedPackageModule::RequiredModules */
    std::span<const std::string> VerifiedPackageModule::RequiredModules() const noexcept {
        return state_->requiredModules;
    }

    /** @copydoc VerifiedPackageModule::Activation */
    const Extensions::ExtensionActivationLease &VerifiedPackageModule::Activation() const noexcept {
        return state_->activation;
    }

    /** @copydoc VerifiedPackageModule::Capabilities */
    std::span<const Extensions::ExtensionCapabilityHandle> VerifiedPackageModule::Capabilities() const noexcept {
        return state_->capabilities;
    }

    /** @copydoc VerifiedPackageModule::ReadDeclaredFile */
    Result<std::vector<std::byte>> VerifiedPackageModule::ReadDeclaredFile(const PackagePath &path,
                                                                           const std::uint64_t maximumBytes) const {
        if (!Activation().IsUsable())
            return Result<std::vector<std::byte>>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
        const auto &name = state_->descriptor.Value();
        const auto root = name.substr(0, name.rfind('/'));
        const auto entries = Package().archive->Manifest().Entries();
        const auto file = std::ranges::find(entries, path.Value(), [](const auto &entry) {
            return entry.id.path.Value();
        });
        if (file == entries.end() || !file->contributionRoot || file->contributionRoot->Value() != root ||
            !path.Value().starts_with(root + '/'))
            return Result<std::vector<std::byte>>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
        return Package().archive->ReadDeclaredFile(path, maximumBytes);
    }
}  // namespace Horo::Packages
