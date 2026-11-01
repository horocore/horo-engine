#include "AuthenticatedMetalCookConfiguration.h"

#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "HostSignatureDocument.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <fstream>

namespace Horo::Application::Internal {
    using namespace Render;
    using Bytes = Result<std::vector<std::uint8_t>>;

    /** @brief Reads explicit host inputs only, with a finite allocation and exact length before use. */
    Bytes ReadShaderCookInput(const std::filesystem::path &path, const std::size_t maximum) {
        std::error_code error;
        if (!path.is_absolute() || !std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error)
            return Bytes::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0 || size > maximum)
            return Bytes::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        std::ifstream stream(path, std::ios::binary);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!stream.read(std::bit_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())) ||
            stream.peek() != std::char_traits<char>::eof())
            return Bytes::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        return Bytes::Success(std::move(bytes));
    }

    /** @brief The executable's real cook host identity cannot be overridden by catalog or command-line data. */
    std::string_view ActualMetalCookHost() noexcept {
#if defined(__APPLE__) && defined(__aarch64__)
        return "macos-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
        return "macos-x86_64";
#else
        return {};
#endif
    }

    namespace {
        /** @brief Constructs only independent explicitly supplied active roots; decoding alone does not approve a publisher. */
        Result<std::shared_ptr<Security::TrustedRootStore>> Roots(const std::span<const std::uint8_t> bytes) {
            using Creation = Result<std::shared_ptr<Security::TrustedRootStore>>;
            const auto policy = DecodePublisherTrustDocument(std::as_bytes(bytes), Packages::PackagePublisherVerificationPolicy{});
            if (policy.HasError())
                return Creation::Failure(policy.ErrorValue());
            auto roots = std::make_shared<Security::TrustedRootStore>();
            const auto now = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            for (const auto &record : policy.Value().publishers) {
                if (record.publisherRevoked || (record.expiresAtUnixMilliseconds != 0 && now >= record.expiresAtUnixMilliseconds))
                    continue;
                const auto added = roots->Add({record.algorithm, record.publisher.Value(), record.keyId, record.publicKey});
                if (added.HasError())
                    return Creation::Failure(added.ErrorValue());
            }
            return Creation::Success(std::move(roots));
        }

        struct CatalogSnapshot {
            std::shared_ptr<const VerifiedShaderHostCatalog> approval;
            std::vector<std::uint8_t> catalog, signature;
        };

        Result<CatalogSnapshot> Authenticate(const AuthenticatedMetalCookInputs &inputs) {
            using Catalog = Result<CatalogSnapshot>;
            auto catalogBytes = ReadShaderCookInput(inputs.catalog, 65536);
            auto signatureBytes = ReadShaderCookInput(inputs.detachedSignature, MaximumHostSignatureDocumentBytes);
            const auto trustBytes = ReadShaderCookInput(inputs.hostTrust, MaximumHostSignatureDocumentBytes);
            if (catalogBytes.HasError())
                return Catalog::Failure(catalogBytes.ErrorValue());
            if (signatureBytes.HasError())
                return Catalog::Failure(signatureBytes.ErrorValue());
            if (trustBytes.HasError())
                return Catalog::Failure(trustBytes.ErrorValue());
            const auto envelope = DecodeDetachedSignatureDocument(std::as_bytes(std::span{signatureBytes.Value()}));
            if (envelope.HasError())
                return Catalog::Failure(envelope.ErrorValue());
            const auto roots = Roots(trustBytes.Value());
            if (roots.HasError())
                return Catalog::Failure(roots.ErrorValue());
            const Security::ArtifactVerifier verifier{Security::CreateMbedTlsSignatureProvider(), roots.Value()};
            const auto approved = VerifiedShaderHostCatalog::Create(std::as_bytes(std::span{catalogBytes.Value()}), envelope.Value(),
                                                                    verifier, ActualMetalCookHost());
            if (approved.HasError())
                return Catalog::Failure(approved.ErrorValue());
            return Catalog::Success({approved.Value(), std::move(catalogBytes).Value(), std::move(signatureBytes).Value()});
        }

        Result<void> InstallTools(ShaderCompilerToolchainConfiguration &configuration, const VerifiedShaderHostCatalog &catalog,
                                  const AuthenticatedMetalCookInputs &inputs) {
            constexpr std::array roles{ShaderCompilerTool::Dxc, ShaderCompilerTool::SpirvTools, ShaderCompilerTool::SpirvCross,
                                       ShaderCompilerTool::AppleMetal};
            const auto approved = catalog.Tools();
            for (std::size_t index = 0; index < roles.size(); ++index) {
                const auto entry = std::ranges::find(approved, roles[index], [](const auto &tool) {
                    return tool.identity.tool;
                });
                if (entry == approved.end() || !inputs.executables[index].is_absolute())
                    return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
                configuration.tools.emplace_back(entry->identity, inputs.executables[index], entry->executableDigest);
            }
            return Result<void>::Success();
        }
    }  // namespace

    Result<AuthenticatedMetalCookConfiguration> MakeAuthenticatedMetalCookConfiguration(const AuthenticatedMetalCookInputs &inputs) {
        using Configuration = Result<AuthenticatedMetalCookConfiguration>;
        if (ActualMetalCookHost().empty())
            return Configuration::Failure(MakeError(ShaderCompilerPipelineErrors::UnsupportedTarget));
        auto catalog = Authenticate(inputs);
        if (catalog.HasError())
            return Configuration::Failure(catalog.ErrorValue());
        ShaderCompilerToolchainConfiguration configuration{.hostPlatform = std::string{ActualMetalCookHost()},
                                                           .scratchRoot = inputs.scratchRoot,
                                                           .verifiedCatalog = catalog.Value().approval,
                                                           .processTimeout = std::chrono::seconds{60}};
        if (const auto installed = InstallTools(configuration, *catalog.Value().approval, inputs); installed.HasError())
            return Configuration::Failure(installed.ErrorValue());
        return Configuration::Success({std::move(configuration),
                                       std::make_shared<const std::vector<std::uint8_t>>(std::move(catalog.Value().catalog)),
                                       std::make_shared<const std::vector<std::uint8_t>>(std::move(catalog.Value().signature))});
    }
}  // namespace Horo::Application::Internal
