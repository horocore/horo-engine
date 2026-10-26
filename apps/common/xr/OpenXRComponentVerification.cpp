#include "Horo/XR/XRErrors.h"
#include "OpenXRComponentManifest.h"

#include <algorithm>
#include <array>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Installation admission failure; original archive failures remain intact. */
        Error InvalidInstall() {
            return MakeError(XR::XRErrors::LoaderPreflightInvalid);
        }

        /** @brief Checks exact host first-party signing policy against the sealed archive decision. */
        bool HasPublisher(const Packages::PackageRestorePackage &package, const OpenXRComponentHostPolicy &policy) {
            return package.archive && package.archive->Digest() == package.lock.artifactDigest && package.publisher &&
                   package.publisher->outcome == Packages::PackagePublisherVerificationOutcome::Accepted &&
                   package.publisher->installPermitted && package.publisher->publisher == policy.publisher && !policy.signingKey.empty() &&
                   package.publisher->keyId == policy.signingKey && package.publisher->artifactDigest == package.archive->Digest();
        }

        /** @brief Validates exact ABI and bridge requirements without selecting any provider or opening native code. */
        Result<void> Compatible(const OpenXRComponentManifest &manifest, const OpenXRComponentHostPolicy &policy) {
            if (manifest.platform != policy.platform || manifest.identity.backend != policy.backend ||
                manifest.identity.platformBridge != policy.platformBridge || manifest.identity.rendererBridge != policy.rendererBridge)
                return Result<void>::Failure(MakeError(XR::XRErrors::OperationIncompatible));
            return XR::RequireXRContractVersion(policy.contract, manifest.identity.contract);
        }

        /** @brief Reads only the exact descriptor declared inside its explicit contribution root. */
        Result<std::vector<std::byte>> DescriptorBytes(const Packages::PackageRestorePackage &package,
                                                       const Packages::PackagePath &descriptor) {
            const auto slash = descriptor.Value().rfind('/');
            const auto entries = package.archive->Manifest().Entries();
            if (const auto declared = std::ranges::find_if(entries,
                                                           [&descriptor](const auto &entry) {
                return entry.id.path.Value() == descriptor.Value();
            });
                slash == std::string::npos || slash == 0 || declared == entries.end() || !declared->contributionRoot ||
                declared->contributionRoot->Value() != descriptor.Value().substr(0, slash))
                return Result<std::vector<std::byte>>::Failure(InvalidInstall());
            return package.archive->ReadDeclaredFile(descriptor, 64U * 1024U);
        }

        /** @brief Verifies actual declared file bytes, not merely digest claims or arbitrary filesystem paths. */
        Result<void> VerifyFile(const Packages::PackageRestorePackage &package, const OpenXRComponentFile &file,
                                const std::string_view root, const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(XR::XRErrors::LoaderPreflightCancelled));
            const auto entries = package.archive->Manifest().Entries();
            if (const auto found = std::ranges::find_if(entries,
                                                        [&file](const auto &entry) {
                return entry.id.path.Value() == file.path.Value();
            });
                found == entries.end() || !found->executable || !found->contributionRoot || found->contributionRoot->Value() != root ||
                found->digest != file.digest || found->size > 64U * 1024U * 1024U)
                return Result<void>::Failure(InvalidInstall());
            auto bytes = package.archive->ReadDeclaredFile(file.path, 64U * 1024U * 1024U);
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            if (ComputeSha256(bytes.Value()) != file.digest)
                return Result<void>::Failure(InvalidInstall());
            return Result<void>::Success();
        }

        /** @brief Binds all component evidence and the service-issued install revision with canonical byte ordering. */
        Sha256Digest BoundIdentity(const Packages::VerifiedPackageInstallRecord &install, const Packages::PackageRestorePackage &package,
                                   const OpenXRComponentManifest &manifest) {
            std::array<std::byte, 8 + 32 * 3> bytes{};
            for (std::size_t index = 0; index < 8; ++index)
                bytes[index] = static_cast<std::byte>(install.Revision() >> ((7 - index) * 8));
            std::size_t offset = 8;
            for (const auto *digest : {&install.Graph()->requestHash, &package.lock.artifactDigest, &manifest.descriptorDigest}) {
                for (const auto value : digest->bytes)
                    bytes[offset++] = static_cast<std::byte>(value);
            }
            return ComputeSha256(bytes);
        }

        /** @brief Issues a compact domain identity while retaining the full digest for every exact candidate comparison. */
        Result<XR::XRInstallRecordId> CompactIdentity(const Sha256Digest &digest) {
            std::uint64_t value{};
            for (std::size_t index = 0; index < 8; ++index)
                value = (value << 8) | digest.bytes[index];
            return XR::XRInstallRecordId::Create(value);
        }
    }  // namespace

    /** @copydoc VerifiedOpenXRComponent::Verify */
    Result<VerifiedOpenXRComponent> VerifiedOpenXRComponent::Verify(std::shared_ptr<const Packages::VerifiedPackageInstallRecord> install,
                                                                    const Packages::HoroPackageId &package,
                                                                    const Packages::PackagePath &descriptor,
                                                                    const OpenXRComponentHostPolicy &policy,
                                                                    const CancellationToken &cancellation) {
        using Outcome = Result<VerifiedOpenXRComponent>;
        if (!install || !install->Graph() || install->Revision() == 0 || install->Graph()->platform != policy.platform ||
            !policy.backend.IsValid() || policy.platformBridge == 0 || policy.rendererBridge == 0)
            return Outcome::Failure(InvalidInstall());
        const auto &packages = install->Graph()->packages;
        const auto selected = std::ranges::find(packages, package, [](const auto &entry) {
            return entry.lock.package;
        });
        if (selected == packages.end() ||
            std::ranges::count(packages, package,
                               [](const auto &entry) {
            return entry.lock.package;
        }) != 1 ||
            !HasPublisher(*selected, policy))
            return Outcome::Failure(InvalidInstall());
        const auto slash = descriptor.Value().rfind('/');
        if (slash == std::string::npos || slash == 0 || cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(cancellation.IsCancellationRequested() ? XR::XRErrors::LoaderPreflightCancelled
                                                                                     : XR::XRErrors::LoaderPreflightInvalid));
        auto bytes = DescriptorBytes(*selected, descriptor);
        if (bytes.HasError())
            return Outcome::Failure(bytes.ErrorValue());
        auto manifest = ParseOpenXRComponentManifest(bytes.Value());
        if (manifest.HasError())
            return Outcome::Failure(manifest.ErrorValue());
        if (const auto compatible = Compatible(manifest.Value(), policy); compatible.HasError())
            return Outcome::Failure(compatible.ErrorValue());
        const auto root = descriptor.Value().substr(0, slash);
        if (const auto loader = VerifyFile(*selected, manifest.Value().loader, root, cancellation); loader.HasError())
            return Outcome::Failure(loader.ErrorValue());
        for (const auto &file : manifest.Value().dependencies)
            if (const auto verified = VerifyFile(*selected, file, root, cancellation); verified.HasError())
                return Outcome::Failure(verified.ErrorValue());
        const auto identity = BoundIdentity(*install, *selected, manifest.Value());
        auto compact = CompactIdentity(identity);
        if (compact.HasError())
            return Outcome::Failure(compact.ErrorValue());
        const auto index = static_cast<std::size_t>(selected - packages.begin());
        return Outcome::Success(
            VerifiedOpenXRComponent{std::move(install), index, descriptor, std::move(manifest).Value(), identity, compact.Value()});
    }

    /** @copydoc VerifiedOpenXRComponent::VerifiedOpenXRComponent */
    VerifiedOpenXRComponent::VerifiedOpenXRComponent(std::shared_ptr<const Packages::VerifiedPackageInstallRecord> install,
                                                     const std::size_t index, Packages::PackagePath descriptor,
                                                     OpenXRComponentManifest manifest, const Sha256Digest &identity,
                                                     const XR::XRInstallRecordId installIdentity)
        : install_(std::move(install)), packageIndex_(index), descriptor_(std::move(descriptor)), manifest_(std::move(manifest)),
          identity_(identity), installIdentity_(installIdentity) {}

    /** @copydoc VerifiedOpenXRComponent::Install */
    const std::shared_ptr<const Packages::VerifiedPackageInstallRecord> &VerifiedOpenXRComponent::Install() const noexcept {
        return install_;
    }

    /** @copydoc VerifiedOpenXRComponent::PackageIndex */
    std::size_t VerifiedOpenXRComponent::PackageIndex() const noexcept {
        return packageIndex_;
    }

    /** @copydoc VerifiedOpenXRComponent::Descriptor */
    const Packages::PackagePath &VerifiedOpenXRComponent::Descriptor() const noexcept {
        return descriptor_;
    }

    /** @copydoc VerifiedOpenXRComponent::Manifest */
    const OpenXRComponentManifest &VerifiedOpenXRComponent::Manifest() const noexcept {
        return manifest_;
    }

    /** @copydoc VerifiedOpenXRComponent::IdentityDigest */
    const Sha256Digest &VerifiedOpenXRComponent::IdentityDigest() const noexcept {
        return identity_;
    }

    /** @copydoc VerifiedOpenXRComponent::InstallIdentity */
    XR::XRInstallRecordId VerifiedOpenXRComponent::InstallIdentity() const noexcept {
        return installIdentity_;
    }
}  // namespace Horo::Application::Internal
