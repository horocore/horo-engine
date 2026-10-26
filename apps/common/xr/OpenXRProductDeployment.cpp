#include "OpenXRProductDeployment.h"

#include "Horo/XR/XRErrors.h"
#include "JsonUtils.h"

#include <bit>

namespace Horo::Application::Internal {
    namespace {
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
            const auto &platform = root.at("platform");
            if (platform.at("os") != policy.platform.operatingSystem || platform.at("architecture") != policy.platform.architecture ||
                platform.at("sdkAbi") != policy.platform.sdkAbi || !root.at("bundledDependencies").is_array() ||
                !root.at("bundledDependencies").empty() || !manifest.dependencies.empty())
                return false;
            const auto &dependencies = root.at("osAbiDependencies");
            if (!dependencies.is_array() || dependencies.empty() || dependencies.size() > 32)
                return false;
            for (const auto &dependency : dependencies) {
                if (!dependency.is_string())
                    return false;
                const auto &name = dependency.get_ref<const std::string &>();
                if (name.empty() || name.size() > 128 || !std::ranges::all_of(name, [](const unsigned char character) {
                    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '.' || character == '_' || character == '-';
                }))
                    return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc VerifiedOpenXRDeployment::Verify */
    Result<VerifiedOpenXRDeployment> VerifiedOpenXRDeployment::Verify(const VerifiedOpenXRComponent &component,
                                                                      const std::span<const std::byte> bytes,
                                                                      const Security::DetachedSignatureEnvelope &envelope,
                                                                      const Security::ArtifactVerifier &verifier,
                                                                      const OpenXRComponentHostPolicy &policy) {
        using Outcome = Result<VerifiedOpenXRDeployment>;
        const auto &publisher = component.Install()->Graph()->packages[component.PackageIndex()].publisher;
        if (bytes.empty() || bytes.size() > 16U * 1024U || policy.deploymentContract.empty() || !publisher ||
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
        } catch (const Foundation::Json::exception &) {
            return Outcome::Failure(MakeError(XR::XRErrors::OperationUnavailable));
        }
        return Outcome::Success(VerifiedOpenXRDeployment{component.IdentityDigest(), std::move(evidence).Value()});
    }

    /** @copydoc VerifiedOpenXRDeployment::VerifiedOpenXRDeployment */
    VerifiedOpenXRDeployment::VerifiedOpenXRDeployment(const Sha256Digest component, Security::VerifiedArtifactEvidence evidence)
        : component_(component), evidence_(std::move(evidence)) {}

    /** @copydoc VerifiedOpenXRDeployment::ComponentIdentity */
    const Sha256Digest &VerifiedOpenXRDeployment::ComponentIdentity() const noexcept {
        return component_;
    }

    /** @copydoc VerifiedOpenXRDeployment::Evidence */
    const Security::VerifiedArtifactEvidence &VerifiedOpenXRDeployment::Evidence() const noexcept {
        return evidence_;
    }
}  // namespace Horo::Application::Internal
