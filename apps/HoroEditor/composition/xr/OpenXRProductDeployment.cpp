#include "OpenXRProductDeployment.h"

#include "Horo/XR/XRErrors.h"
#include "JsonUtils.h"
#include "OpenXRWindowsLoaderImage.h"

#include <algorithm>
#include <bit>
#include <new>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Retains the finite unique signed names with Windows' exact case-insensitive spelling rules. */
        Result<std::vector<std::string>> ReadOsAbiDependencies(const Foundation::Json &dependencies, bool windows) {
            using Names = Result<std::vector<std::string>>;
            const auto invalid = [] {
                return Names::Failure(MakeError(XR::XRErrors::OperationUnavailable, "Invalid signed OpenXR OS ABI dependency inventory."));
            };
            if (!dependencies.is_array() || dependencies.empty() || dependencies.size() > MaximumOpenXRAbiDependencies)
                return invalid();
            std::vector<std::string> names;
            names.reserve(dependencies.size());
            for (const auto &entry : dependencies) {
                if (!entry.is_string())
                    return invalid();
                auto name = entry.get<std::string>();
                if (windows) {
                    if (!IsWindowsOpenXRAbiDll(name))
                        return invalid();
                    for (auto &character : name)
                        if (character >= 'A' && character <= 'Z')
                            character = static_cast<char>(character + ('a' - 'A'));
                } else if (name.empty() || name.size() > 128 || !std::ranges::all_of(name, [](const unsigned char character) {
                    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
                })) {
                    return invalid();
                }
                if (std::ranges::find(names, name) != names.end())
                    return invalid();
                names.push_back(std::move(name));
            }
            return Names::Success(std::move(names));
        }

        /** @brief Matches signed deployment facts to the exact component and installer-owned contract. */
        bool Matches(const Foundation::Json &root, const VerifiedOpenXRComponent &component, const OpenXRComponentHostPolicy &policy) {
            const auto &manifest = component.Manifest();
            if (!Foundation::HasAllowedFields(root, {"schemaVersion", "backend", "build", "releaseChannel", "platform",
                                                     "deploymentContract", "loaderDigest", "bundledDependencies", "osAbiDependencies"}) ||
                !root.at("schemaVersion").is_number_unsigned() || root.at("schemaVersion") != 1 ||
                !root.at("backend").is_number_unsigned() || root.at("backend").get<std::uint64_t>() != policy.backend.Value() ||
                root.at("build") != manifest.identity.build || root.at("releaseChannel") != manifest.identity.releaseChannel ||
                root.at("deploymentContract") != policy.deploymentContract ||
                root.at("loaderDigest") != FormatSha256(manifest.loader.digest) ||
                !Foundation::HasAllowedFields(root.at("platform"), {"os", "architecture", "sdkAbi"}))
                return false;
            if (const auto &platform = root.at("platform");
                platform.at("os") != policy.platform.operatingSystem || platform.at("architecture") != policy.platform.architecture ||
                platform.at("sdkAbi") != policy.platform.sdkAbi || !root.at("bundledDependencies").is_array() ||
                !root.at("bundledDependencies").empty() || !manifest.dependencies.empty())
                return false;
            return true;
        }

        /** @brief Binds Windows direct import evidence to exact already verified component archive bytes before any file is staged. */
        Result<void> VerifyWindowsImage(const VerifiedOpenXRComponent &component, std::span<const std::string> names) {
            const auto &manifest = component.Manifest();
            const auto &archive = component.Install()->Graph()->packages[component.PackageIndex()].archive;
            const auto bytes = archive->ReadDeclaredFile(manifest.loader.path, MaximumOpenXRLoaderImageBytes);
            if (bytes.HasError())
                return Result<void>::Failure(bytes.ErrorValue());
            if (ComputeSha256(bytes.Value()) != manifest.loader.digest)
                return Result<void>::Failure(
                    MakeError(XR::XRErrors::RuntimeRejected, "Windows loader bytes changed after component admission."));
            return VerifyWindowsOpenXRLoaderImage(bytes.Value(), manifest.platform.architecture, names);
        }
    }  // namespace

    /** @copydoc VerifiedOpenXRDeployment::Verify */
    Result<VerifiedOpenXRDeployment> VerifiedOpenXRDeployment::Verify(const VerifiedOpenXRComponent &component,
                                                                      const std::span<const std::byte> bytes,
                                                                      const Security::DetachedSignatureEnvelope &envelope,
                                                                      const Security::ArtifactVerifier &verifier,
                                                                      const OpenXRComponentHostPolicy &policy) {
        using Outcome = Result<VerifiedOpenXRDeployment>;
        if (const auto &publisher = component.Install()->Graph()->packages[component.PackageIndex()].publisher;
            bytes.empty() || bytes.size() > 16U * 1024U || policy.deploymentContract.empty() || !publisher ||
            publisher->publisher != policy.publisher || publisher->keyId != policy.signingKey ||
            envelope.publisherId != policy.publisher.Value() || envelope.keyId != policy.signingKey ||
            component.Manifest().platform != policy.platform || component.Manifest().identity.backend != policy.backend)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        auto evidence = verifier.Verify(bytes, envelope);
        if (evidence.HasError())
            return Outcome::Failure(evidence.ErrorValue());
        if (evidence.Value().ArtifactDigest() != component.Manifest().provenance)
            return Outcome::Failure(MakeError(XR::XRErrors::OperationIncompatible));
        try {
            Foundation::JsonParseGuard guard{6};
            const auto *text = std::bit_cast<const char *>(bytes.data());
            const auto root = guard.Parse(text, text + bytes.size());
            if (guard.HasDuplicate() || guard.IsTooDeep() || !Matches(root, component, policy))
                return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable));
            const bool windows = policy.platform.operatingSystem == "windows";
            auto names = ReadOsAbiDependencies(root.at("osAbiDependencies"), windows);
            if (names.HasError())
                return Outcome::Failure(names.ErrorValue());
            if (windows) {
                if (const auto image = VerifyWindowsImage(component, names.Value()); image.HasError())
                    return Outcome::Failure(image.ErrorValue());
            }
            return Outcome::Success(
                VerifiedOpenXRDeployment{component.IdentityDigest(), std::move(evidence).Value(), std::move(names).Value()});
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(
                MakeError(XR::XRErrors::CapacityExceeded, "Insufficient memory for signed OpenXR deployment evidence."));
        } catch (const Foundation::Json::exception &) {
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        }
    }

    /** @copydoc VerifiedOpenXRDeployment::VerifiedOpenXRDeployment */
    VerifiedOpenXRDeployment::VerifiedOpenXRDeployment(const Sha256Digest &component, Security::VerifiedArtifactEvidence evidence,
                                                       std::vector<std::string> osAbiDependencies)
        : component_(component), evidence_(std::move(evidence)), osAbiDependencies_(std::move(osAbiDependencies)) {}

    /** @copydoc VerifiedOpenXRDeployment::ComponentIdentity */
    const Sha256Digest &VerifiedOpenXRDeployment::ComponentIdentity() const noexcept {
        return component_;
    }

    /** @copydoc VerifiedOpenXRDeployment::Evidence */
    const Security::VerifiedArtifactEvidence &VerifiedOpenXRDeployment::Evidence() const noexcept {
        return evidence_;
    }

    /** @copydoc VerifiedOpenXRDeployment::OsAbiDependencyCount */
    std::uint8_t VerifiedOpenXRDeployment::OsAbiDependencyCount() const noexcept {
        return static_cast<std::uint8_t>(osAbiDependencies_.size());
    }

    /** @copydoc VerifiedOpenXRDeployment::OsAbiDependencies */
    std::span<const std::string> VerifiedOpenXRDeployment::OsAbiDependencies() const noexcept {
        return osAbiDependencies_;
    }
}  // namespace Horo::Application::Internal
