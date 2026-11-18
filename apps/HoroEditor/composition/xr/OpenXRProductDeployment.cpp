#include "OpenXRProductDeployment.h"

#include "Horo/XR/XRErrors.h"
#include "JsonUtils.h"

#include <algorithm>
#include <array>
#include <bit>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Accepts only a finite unique list of signed OS ABI names. */
        bool ValidOsAbiDependencies(const Foundation::Json &dependencies) {
            std::array<std::string_view, 32> names{};
            if (!dependencies.is_array() || dependencies.empty() || dependencies.size() > names.size())
                return false;
            std::size_t count = 0;
            for (const auto &entry : dependencies) {
                if (!entry.is_string())
                    return false;
                const std::string_view name = entry.get_ref<const std::string &>();
                const auto existing = std::span{names}.first(count);
                if (name.empty() || name.size() > 128 || std::ranges::find(existing, name) != existing.end() ||
                    !std::ranges::all_of(name, [](const unsigned char character) {
                    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
                }))
                    return false;
                names[count++] = name;
            }
            return true;
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
            return ValidOsAbiDependencies(root.at("osAbiDependencies"));
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
            const auto dependencyCount = static_cast<std::uint8_t>(root.at("osAbiDependencies").size());
            return Outcome::Success(VerifiedOpenXRDeployment{component.IdentityDigest(), std::move(evidence).Value(), dependencyCount});
        } catch (const Foundation::Json::exception &) {
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        }
    }

    /** @copydoc VerifiedOpenXRDeployment::VerifiedOpenXRDeployment */
    VerifiedOpenXRDeployment::VerifiedOpenXRDeployment(const Sha256Digest &component, Security::VerifiedArtifactEvidence evidence,
                                                       std::uint8_t osAbiDependencies)
        : component_(component), evidence_(std::move(evidence)), osAbiDependencies_(osAbiDependencies) {}

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
        return osAbiDependencies_;
    }
}  // namespace Horo::Application::Internal
