#pragma once

/** @file OpenXRComponentManifest.h
 * @brief Non-installed first-party XR artifact admission; no backend activation or native loading.
 */

#include "Horo/Packages/PackageInstall.h"
#include "Horo/XR/XRLoaderPreflight.h"

#include <span>
#include <string>
#include <vector>

namespace Horo::Application::Internal {
    /** @brief One exact declared artifact; a digest claim becomes evidence only through the sealed archive. */
    struct OpenXRComponentFile final {
        Packages::PackagePath path;
        Sha256Digest digest;
    };

    /** @brief Immutable product identity and private bridge compatibility requirements. */
    struct OpenXRComponentIdentity final {
        XR::XRBackendId backend;
        XR::XRContractVersion contract;
        std::string build;
        std::string releaseChannel;
        std::uint32_t platformBridge{};
        std::uint32_t rendererBridge{};
    };

    /** @brief Strict bounded deployment metadata; it grants neither trust nor native execution. */
    struct OpenXRComponentManifest final {
        OpenXRComponentIdentity identity;
        Packages::PackagePlatform platform;
        OpenXRComponentFile loader;
        XR::XRLoaderApiVersionRange loaderVersions;
        std::vector<OpenXRComponentFile> dependencies;
        Sha256Digest licenses;
        Sha256Digest provenance;
        Sha256Digest descriptorDigest;
    };

    /** @brief Executable-owned first-party policy, independent of project package acceptance. */
    struct OpenXRComponentHostPolicy final {
        Packages::PackagePlatform platform;
        XR::XRBackendId backend;
        XR::XRContractVersion contract{XR::CurrentXRContractVersion};
        Packages::PackagePublisherId publisher;
        std::string signingKey;
        std::string deploymentContract; /**< Exact installer-owned qualified deployment contract; empty denies native admission. */
        std::uint32_t platformBridge{};
        std::uint32_t rendererBridge{};
    };

    /** @brief Parses exact schema-one metadata with finite size, depth and dependency limits.
     * @param bytes Verified archive descriptor bytes; parsing alone confers no trust.
     * @return Complete inert manifest or a typed XR installation-admission diagnostic.
     * @post Does not inspect the filesystem, open a library, discover a runtime or select a backend.
     */
    [[nodiscard]] Result<OpenXRComponentManifest> ParseOpenXRComponentManifest(std::span<const std::byte> bytes);

    /** @brief Sealed first-party component candidate retaining the actual package installation authority.
     * @details Only Verify can issue this value. It binds exact descriptor/file digests, publisher policy and
     * installation revision. Native artifact signatures and dependency-load admission remain separate mandatory gates.
     */
    class VerifiedOpenXRComponent final {
    public:
        /** @brief Admits one exact host-selected descriptor from an already committed verified installation.
         * @param install Sealed install owner, retained by the result.
         * @param package Exact product-selected package; project metadata cannot approve its publisher.
         * @param descriptor Exact manifest-declared component descriptor.
         * @param policy Explicit executable first-party publisher, ABI and bridge policy.
         * @param cancellation Cancellation before publication.
         * @return Sealed candidate or original typed archive/cancellation/admission failure.
         * @post No staging, native loading, runtime discovery, activation or ambient registration occurs.
         */
        [[nodiscard]] static Result<VerifiedOpenXRComponent> Verify(std::shared_ptr<const Packages::VerifiedPackageInstallRecord> install,
                                                                    const Packages::HoroPackageId &package,
                                                                    const Packages::PackagePath &descriptor,
                                                                    const OpenXRComponentHostPolicy &policy,
                                                                    const CancellationToken &cancellation);

        /** @brief Returns the exact sealed installation owner. @return Owning immutable record. */
        [[nodiscard]] const std::shared_ptr<const Packages::VerifiedPackageInstallRecord> &Install() const noexcept;
        /** @brief Returns the selected index in Install()->Graph(). @return Validated immutable index. */
        [[nodiscard]] std::size_t PackageIndex() const noexcept;
        /** @brief Returns the exact declared descriptor identity. @return Borrowed immutable identity. */
        [[nodiscard]] const Packages::PackagePath &Descriptor() const noexcept;
        /** @brief Returns verified deployment metadata. @return Borrowed immutable manifest. */
        [[nodiscard]] const OpenXRComponentManifest &Manifest() const noexcept;
        /** @brief Returns the full bound installation digest; no truncated ID alone proves equality. */
        [[nodiscard]] const Sha256Digest &IdentityDigest() const noexcept;
        /** @brief Returns the XR preflight identity derived from the retained full digest. */
        [[nodiscard]] XR::XRInstallRecordId InstallIdentity() const noexcept;

    private:
        /** @brief Retains only factory-verified installation and exact bound component evidence. */
        VerifiedOpenXRComponent(std::shared_ptr<const Packages::VerifiedPackageInstallRecord> install, std::size_t index,
                                Packages::PackagePath descriptor, OpenXRComponentManifest manifest, const Sha256Digest &identity,
                                XR::XRInstallRecordId installIdentity);
        std::shared_ptr<const Packages::VerifiedPackageInstallRecord> install_;
        std::size_t packageIndex_{};
        Packages::PackagePath descriptor_;
        OpenXRComponentManifest manifest_;
        Sha256Digest identity_;
        XR::XRInstallRecordId installIdentity_;
    };
}  // namespace Horo::Application::Internal
