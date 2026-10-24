#include "OpenXRInstalledHostCatalog.h"

#include "Horo/XR/XRFeatureNegotiation.h"
#include "HostSignatureDocument.h"
#include "JsonUtils.h"

#include <algorithm>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Keeps malformed installer selection distinct from real signature/native/installation failure. */
        Error InvalidCatalog() {
            return MakeError(XR::XRErrors::LoaderPreflightInvalid,
                             "Installed XR catalog is malformed, expired or outside the implemented host contract.");
        }

        /** @brief The signed expected deployment contract is a bounded identity, never a path or loader search expression. */
        bool Contract(const Foundation::Json &value) {
            if (!value.is_string())
                return false;
            const auto &text = value.get_ref<const std::string &>();
            return !text.empty() && text.size() <= 128 && std::ranges::all_of(text, [](const unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
            });
        }

        /** @brief Extracts one existing envelope codec result and binds it to the independently verified catalog publisher. */
        Result<Security::DetachedSignatureEnvelope> Envelope(const Foundation::Json &value,
                                                             const Security::VerifiedArtifactEvidence &evidence) {
            using Outcome = Result<Security::DetachedSignatureEnvelope>;
            if (!value.is_string())
                return Outcome::Failure(InvalidCatalog());
            const auto &text = value.get_ref<const std::string &>();
            auto decoded = DecodeDetachedSignatureDocument(std::as_bytes(std::span{text.data(), text.size()}));
            if (decoded.HasError())
                return decoded;
            if (decoded.Value().publisherId != evidence.PublisherId() || decoded.Value().keyId != evidence.KeyId())
                return Outcome::Failure(InvalidCatalog());
            return decoded;
        }

        /** @brief Rejects oversized signed budgets before narrowing; capacity is intent, never runtime evidence. */
        bool Limit(const Foundation::Json &value, const std::uint32_t maximum) {
            return value.is_number_unsigned() && value.get<std::uint64_t>() > 0 && value.get<std::uint64_t>() <= maximum;
        }
    }  // namespace

    /** @copydoc OpenXRInstalledHostCatalog::Verify */
    Result<OpenXRInstalledHostCatalog> OpenXRInstalledHostCatalog::Verify(const std::span<const std::byte> bytes,
                                                                          const Security::DetachedSignatureEnvelope &signature,
                                                                          std::shared_ptr<const Security::ArtifactVerifier> verifier,
                                                                          const Packages::PackagePlatform &platform,
                                                                          const std::filesystem::path &temporaryParent,
                                                                          const std::uint64_t nowUnixSeconds) {
        using Outcome = Result<OpenXRInstalledHostCatalog>;
        if (!verifier || bytes.empty() || bytes.size() > 64U * 1024U || temporaryParent.empty() || !temporaryParent.is_absolute() ||
            temporaryParent != temporaryParent.lexically_normal())
            return Outcome::Failure(InvalidCatalog());
        auto evidence = verifier->Verify(bytes, signature);
        if (evidence.HasError())
            return Outcome::Failure(evidence.ErrorValue());
        try {
            Foundation::JsonParseGuard guard{6};
            const auto first = reinterpret_cast<const char *>(bytes.data());
            const auto root = guard.Parse(first, first + bytes.size());
            if (guard.HasDuplicate() || guard.IsTooDeep() ||
                !Foundation::HasAllowedFields(root, {"schemaVersion", "backend", "package", "component", "deployment",
                                                     "deploymentSignature", "loaderSignature", "bindings", "deploymentContract",
                                                     "platformBridge", "rendererBridge", "expiresAt", "profile", "limits"}) ||
                !root.at("schemaVersion").is_number_unsigned() || root.at("schemaVersion") != 1 ||
                !root.at("backend").is_number_unsigned() || !root.at("package").is_string() || !root.at("component").is_string() ||
                !root.at("deployment").is_string() || !root.at("bindings").is_string() || !Contract(root.at("deploymentContract")) ||
                !root.at("platformBridge").is_number_unsigned() || root.at("platformBridge") != 1 ||
                !root.at("rendererBridge").is_number_unsigned() || root.at("rendererBridge") != 1 ||
                !root.at("expiresAt").is_number_unsigned() || root.at("expiresAt").get<std::uint64_t>() <= nowUnixSeconds ||
                !root.at("profile").is_number_unsigned() ||
                root.at("profile").get<std::uint64_t>() >= static_cast<std::uint64_t>(XR::XRFeatureProfile::Count) ||
                !Foundation::HasAllowedFields(root.at("limits"), {"views", "spaces", "actions", "devices"}))
                return Outcome::Failure(InvalidCatalog());
            const auto &limits = root.at("limits");
            if (!Limit(limits.at("views"), XR::XRHardLimits::MaximumViews) ||
                !Limit(limits.at("spaces"), XR::XRHardLimits::MaximumSpaces) ||
                !Limit(limits.at("actions"), XR::XRHardLimits::MaximumActions) ||
                !Limit(limits.at("devices"), XR::XRHardLimits::MaximumDevices))
                return Outcome::Failure(InvalidCatalog());
            auto backend = XR::XRBackendId::Create(root.at("backend").get<std::uint64_t>());
            auto package = Packages::HoroPackageId::Parse(root.at("package").get<std::string>());
            auto component = Packages::PackagePath::Parse(root.at("component").get<std::string>());
            auto deployment = Packages::PackagePath::Parse(root.at("deployment").get<std::string>());
            auto bindings = Packages::PackagePath::Parse(root.at("bindings").get<std::string>());
            auto publisher = Packages::PackagePublisherId::Parse(evidence.Value().PublisherId());
            auto deploymentSignature = Envelope(root.at("deploymentSignature"), evidence.Value());
            auto loaderSignature = Envelope(root.at("loaderSignature"), evidence.Value());
            if (backend.HasError() || package.HasError() || component.HasError() || deployment.HasError() || bindings.HasError() ||
                publisher.HasError() || deploymentSignature.HasError() || loaderSignature.HasError())
                return Outcome::Failure(InvalidCatalog());
            const auto loaderEnvelope = std::move(loaderSignature).Value();
            auto gate = std::make_shared<Security::DetachedFileArtifactGate>(verifier, [loaderEnvelope](const std::filesystem::path &) {
                return Result<Security::DetachedSignatureEnvelope>::Success(loaderEnvelope);
            });
            OpenXRInstalledProductPolicy policy{std::move(package).Value(),
                                                std::move(component).Value(),
                                                std::move(deployment).Value(),
                                                {platform, backend.Value(), XR::CurrentXRContractVersion, std::move(publisher).Value(),
                                                 std::string(evidence.Value().KeyId()), root.at("deploymentContract").get<std::string>(), 1,
                                                 1},
                                                std::move(deploymentSignature).Value(),
                                                std::move(verifier),
                                                std::move(gate),
                                                temporaryParent};
            return Outcome::Success(
                OpenXRInstalledHostCatalog{std::move(policy),
                                           std::move(bindings).Value(),
                                           std::move(evidence).Value(),
                                           root.at("expiresAt").get<std::uint64_t>(),
                                           {limits.at("views").get<std::uint32_t>(), limits.at("spaces").get<std::uint32_t>(),
                                            limits.at("actions").get<std::uint32_t>(), limits.at("devices").get<std::uint32_t>()},
                                           static_cast<XR::XRFeatureProfile>(root.at("profile").get<std::uint64_t>())});
        } catch (const Foundation::Json::exception &) {
            return Outcome::Failure(InvalidCatalog());
        }
    }

    /** @copydoc OpenXRInstalledHostCatalog::OpenXRInstalledHostCatalog */
    OpenXRInstalledHostCatalog::OpenXRInstalledHostCatalog(OpenXRInstalledProductPolicy policy, Packages::PackagePath bindings,
                                                           Security::VerifiedArtifactEvidence evidence, const std::uint64_t expiresAt,
                                                           XR::XRSystemLimits limits, const XR::XRFeatureProfile profile)
        : policy_(std::move(policy)), bindings_(std::move(bindings)), evidence_(std::move(evidence)), expiresAt_(expiresAt),
          limits_(limits), profile_(profile) {}

    /** @copydoc OpenXRInstalledHostCatalog::Policy */
    const OpenXRInstalledProductPolicy &OpenXRInstalledHostCatalog::Policy() const noexcept {
        return policy_;
    }

    /** @copydoc OpenXRInstalledHostCatalog::Bindings */
    const Packages::PackagePath &OpenXRInstalledHostCatalog::Bindings() const noexcept {
        return bindings_;
    }

    /** @copydoc OpenXRInstalledHostCatalog::Evidence */
    const Security::VerifiedArtifactEvidence &OpenXRInstalledHostCatalog::Evidence() const noexcept {
        return evidence_;
    }

    /** @copydoc OpenXRInstalledHostCatalog::ExpiresAt */
    std::uint64_t OpenXRInstalledHostCatalog::ExpiresAt() const noexcept {
        return expiresAt_;
    }

    /** @copydoc OpenXRInstalledHostCatalog::Limits */
    const XR::XRSystemLimits &OpenXRInstalledHostCatalog::Limits() const noexcept {
        return limits_;
    }

    /** @copydoc OpenXRInstalledHostCatalog::Profile */
    XR::XRFeatureProfile OpenXRInstalledHostCatalog::Profile() const noexcept {
        return profile_;
    }
}  // namespace Horo::Application::Internal
