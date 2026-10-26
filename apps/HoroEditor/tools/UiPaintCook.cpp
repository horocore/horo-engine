/** @file UiPaintCook.cpp @brief Explicit authenticated offline UI Metal producer; runtime never invokes it. */
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "HostSignatureDocument.h"
#include "RuntimeUiPaintShaderPackage.h"
#include "VerifiedShaderHostCatalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Application::Internal;
    using Bytes = Result<std::vector<std::uint8_t>>;

    /** @brief Reads explicit host inputs only, with a finite allocation and exact length before use. */
    Bytes Read(const std::filesystem::path &path, const std::size_t maximum) {
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
    std::string_view ActualHost() {
#if defined(__APPLE__) && defined(__aarch64__)
        return "macos-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
        return "macos-x86_64";
#else
        return {};
#endif
    }

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

    /** @brief Configures the existing adapter from authenticated tuples plus independently supplied absolute tool paths. */
    Result<ShaderCompilerToolchainConfiguration> Configuration(char **argv) {
        using Configuration = Result<ShaderCompilerToolchainConfiguration>;
        if (ActualHost().empty())
            return Configuration::Failure(MakeError(ShaderCompilerPipelineErrors::UnsupportedTarget));
        const auto catalogBytes = Read(argv[4], 65536);
        const auto signatureBytes = Read(argv[5], MaximumHostSignatureDocumentBytes);
        const auto trustBytes = Read(argv[6], MaximumHostSignatureDocumentBytes);
        if (catalogBytes.HasError())
            return Configuration::Failure(catalogBytes.ErrorValue());
        if (signatureBytes.HasError())
            return Configuration::Failure(signatureBytes.ErrorValue());
        if (trustBytes.HasError())
            return Configuration::Failure(trustBytes.ErrorValue());
        const auto envelope = DecodeDetachedSignatureDocument(std::as_bytes(std::span{signatureBytes.Value()}));
        if (envelope.HasError())
            return Configuration::Failure(envelope.ErrorValue());
        const auto roots = Roots(trustBytes.Value());
        if (roots.HasError())
            return Configuration::Failure(roots.ErrorValue());
        const Security::ArtifactVerifier verifier{Security::CreateMbedTlsSignatureProvider(), roots.Value()};
        const auto catalog =
            VerifiedShaderHostCatalog::Create(std::as_bytes(std::span{catalogBytes.Value()}), envelope.Value(), verifier, ActualHost());
        if (catalog.HasError())
            return Configuration::Failure(catalog.ErrorValue());
        ShaderCompilerToolchainConfiguration configuration{.hostPlatform = std::string{ActualHost()},
                                                           .scratchRoot = std::filesystem::path{argv[3]},
                                                           .verifiedCatalog = catalog.Value(),
                                                           .processTimeout = std::chrono::seconds{60}};
        constexpr std::array roles{ShaderCompilerTool::Dxc, ShaderCompilerTool::SpirvTools, ShaderCompilerTool::SpirvCross,
                                   ShaderCompilerTool::AppleMetal};
        const auto approved = catalog.Value()->Tools();
        for (std::size_t index = 0; index < roles.size(); ++index) {
            const auto entry = std::ranges::find(approved, roles[index], [](const auto &tool) {
                return tool.identity.tool;
            });
            const std::filesystem::path executable{argv[7 + index]};
            if (entry == approved.end() || !executable.is_absolute())
                return Configuration::Failure(MakeError(ShaderCompilerPipelineErrors::ToolMissing));
            configuration.tools.emplace_back(entry->identity, executable, entry->executableDigest);
        }
        return Configuration::Success(std::move(configuration));
    }

    /** @brief Writes only a host-claimed private candidate; no existing package file is overwritten. */
    Result<void> Write(const std::filesystem::path &path, const std::span<const std::uint8_t> bytes) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(std::bit_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        stream.close();
        return stream ? Result<void>::Success() : Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
    }

    /** @brief Owns only its successfully claimed directory and abandons it on any pre-publication failure. */
    struct PrivatePackage final {
        std::filesystem::path path;
        bool owned{};

        PrivatePackage() = default;
        PrivatePackage(const PrivatePackage &) = delete;
        PrivatePackage &operator=(const PrivatePackage &) = delete;
        PrivatePackage(PrivatePackage &&) = delete;
        PrivatePackage &operator=(PrivatePackage &&) = delete;

        ~PrivatePackage() {
            if (owned) {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        }
    };

    /** @brief Publishes the completely validated pair by one directory rename into a new host-owned package root.
     * @details The host exclusively owns the output parent during this offline operation; destination must not exist. */
    Result<void> Publish(const std::filesystem::path &destination, const CompiledShaderArtifact &compiled,
                         const std::span<const std::uint8_t> metadata) {
        if (!destination.is_absolute() || destination.filename().empty())
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        std::error_code error;
        if (const bool destinationExists = std::filesystem::exists(destination, error); error || destinationExists)
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        if (const bool parentIsDirectory = std::filesystem::is_directory(destination.parent_path(), error); error || !parentIsDirectory)
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        PrivatePackage candidate;
        for (unsigned attempt = 0; attempt < 1024; ++attempt) {
            candidate.path = destination.parent_path() / std::format(".ui-paint-package-{}", attempt);
            candidate.owned = std::filesystem::create_directory(candidate.path, error);
            if (candidate.owned)
                break;
            if (error && error != std::errc::file_exists)
                return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
            error.clear();
        }
        if (!candidate.owned || !std::filesystem::create_directories(candidate.path / "shaders/runtime", error) || error)
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        if (const auto payload = Write(candidate.path / "shaders/runtime/ui-paint-metal.horoshdr", compiled.payload); payload.HasError())
            return payload;
        if (const auto record = Write(candidate.path / "shaders/runtime/ui-paint-metal.reflection.json", metadata); record.HasError())
            return record;
        std::filesystem::rename(candidate.path, destination, error);
        if (error)
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ScratchIoFailed));
        candidate.owned = false;
        return Result<void>::Success();
    }

    /** @brief Uses the same production adapter, compilation/reflection record and strict runtime package admission. */
    Result<void> Cook(char **argv) {
        auto configuration = Configuration(argv);
        if (configuration.HasError())
            return Result<void>::Failure(configuration.ErrorValue());
        const auto tools = configuration.Value().tools;
        auto adapter =
            ExternalShaderCompilerAdapter::Create(std::move(configuration).Value(), std::make_shared<NativeExternalProcessRunner>());
        if (adapter.HasError())
            return Result<void>::Failure(adapter.ErrorValue());
        auto source = Read(argv[1], 1024U * 1024U);
        if (source.HasError())
            return Result<void>::Failure(source.ErrorValue());
        ShaderCompilerTargetDescriptor target{.requirement = ProjectUiPaintMetalTarget(),
                                              .platformTriple = std::string{ActualHost()},
                                              .intermediateEnvironment = ShaderIntermediateEnvironment::Vulkan13SpirV16};
        for (const auto &tool : tools)
            target.tools.push_back(tool.identity);
        ShaderCompilationRequest request{.manifest = MakeUiPaintShaderManifest(target.requirement),
                                         .source = std::move(source).Value(),
                                         .targets = {target}};
        request.reflection = ShaderCompilerReflectionRequest{tools[2].identity,
                                                             {{{1}, "PaintParameters", {}},
                                                              {{2}, "PaintTexture", {}},
                                                              {{3}, "CoverageTexture", {}},
                                                              {{4}, "PaintSampler", {}}}};
        auto result = CompileShaderTargets(request, adapter.Value(), {});
        if (result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        if (result.Value().artifacts.size() != 1 || !result.Value().artifacts.front().reflection)
            return Result<void>::Failure(MakeError(ShaderCompilerPipelineErrors::ToolOutputInvalid));
        const auto &artifact = result.Value().artifacts.front();
        const auto metadata = EncodeReflectedShaderArtifact(*artifact.reflection);
        if (metadata.HasError())
            return Result<void>::Failure(metadata.ErrorValue());
        if (const auto admitted = DecodeProjectUiPaintMetalPackage(artifact.payload, metadata.Value(), target.requirement.backend);
            admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        return Publish(std::filesystem::path{argv[2]}, artifact, metadata.Value());
    }
}  // namespace

int main(int argc, char **argv) {
    if (argc != 11) {
        std::cerr << "Usage: HoroEditorUiPaintShaderCook ABS-source.hlsl ABS-new-package-root ABS-scratch "
                     "ABS-catalog ABS-detached-signature ABS-host-trust ABS-dxc ABS-spirv-val ABS-spirv-cross ABS-xcrun\n";
        return 2;
    }
    try {
        const auto result = Cook(argv);
        if (result.HasError()) {
            std::cerr << result.ErrorValue().code.Value() << ": " << result.ErrorValue().message << '\n';
            return 1;
        }
    } catch (const std::exception &error) {
        std::cerr << "UI shader cook failed before package publication: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
