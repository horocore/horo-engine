#pragma once
#include "RuntimeUiPaintShaderPackage.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <vector>

namespace Horo::Tests::UiPaintPackageTesting {
    using namespace Render;

    template <typename T> void Append(std::vector<std::uint8_t> &bytes, const T value) {
        for (std::size_t index = 0; index < sizeof(T); ++index)
            bytes.push_back(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
    }

    inline void AppendEntry(std::vector<std::uint8_t> &bytes, const ShaderStage stage, const std::string_view name) {
        Append(bytes, static_cast<std::uint8_t>(stage));
        Append(bytes, static_cast<std::uint32_t>(name.size()));
        bytes.insert(bytes.end(), name.begin(), name.end());
        Append(bytes, std::uint64_t{4});
        bytes.insert(bytes.end(), {1, 2, 3, 4});
    }

    /** @brief Metadata fixture proves package/interface rejection only; its bytes are deliberately not native shader code. */
    inline CookedUiPaintKernel PackageMetadataKernel() {
        CookedUiPaintKernel kernel;
        kernel.target = {ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24, 1, 1, 4, 0, false, false};
        kernel.manifest = MakeUiPaintShaderManifest(kernel.target);
        kernel.artifact.backend = kernel.target.backend;
        kernel.artifact.payloadFormat = kernel.target.payloadFormat;
        kernel.artifact.artifactKey.bytes[0] = 1;
        auto &bytes = kernel.artifact.payload;
        bytes = {'H', 'O', 'R', 'O', 'S', 'H', 'D', 'R'};
        Append(bytes, std::uint32_t{1});
        Append(bytes, static_cast<std::uint8_t>(kernel.target.backend));
        Append(bytes, static_cast<std::uint8_t>(kernel.target.payloadFormat));
        Append(bytes, std::uint32_t{2});
        AppendEntry(bytes, ShaderStage::Vertex, "PaintVertex");
        AppendEntry(bytes, ShaderStage::Fragment, "PaintFragment");
        ShaderReflectionCandidate reflection;
        reflection.backend = kernel.target.backend;
        reflection.interfaceSchemaVersion = 1;
        for (const auto &binding : kernel.manifest.bindings) {
            reflection.bindings.push_back({binding.id, binding.kind, binding.access, 1, binding.stages, true});
            reflection.targetBindings.push_back({.id = binding.id, .nativeBinding = binding.id.value - 1});
        }
        auto normalized = NormalizeShaderReflection(kernel.manifest, kernel.target, reflection);
        REQUIRE(normalized.HasValue());
        Sha256Digest toolDigest;
        toolDigest.bytes[0] = 1;
        auto cooked = CookReflectedShaderArtifact(kernel.artifact, {ShaderCompilerTool::SpirvCross, "unit-fixture-not-native", toolDigest},
                                                  std::move(normalized).Value());
        REQUIRE(cooked.HasValue());
        kernel.artifact.reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(cooked).Value());
        return kernel;
    }

    struct PackageDirectory final {
        std::filesystem::path root;

        PackageDirectory() {
            const auto parent = std::filesystem::temp_directory_path();
            for (unsigned index = 0; index < 1024; ++index) {
                auto candidate = parent / ("horo-ui-package-test-" + std::to_string(index));
                std::error_code error;
                if (std::filesystem::create_directory(candidate, error)) {
                    root = std::move(candidate);
                    std::filesystem::create_directories(root / "shaders/runtime");
                    return;
                }
                REQUIRE((!error || error == std::errc::file_exists));
            }
            FAIL("bounded private package directory claims exhausted");
        }

        ~PackageDirectory() {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }

        PackageDirectory(const PackageDirectory &) = delete;
        PackageDirectory &operator=(const PackageDirectory &) = delete;

        void Write(const std::string_view name, std::span<const std::uint8_t> bytes) const {
            std::ofstream output(root / "shaders/runtime" / name, std::ios::binary);
            REQUIRE(output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
        }

        void PublishMetadataFixture() const {
            const auto kernel = PackageMetadataKernel();
            const auto metadata = EncodeReflectedShaderArtifact(*kernel.artifact.reflection);
            REQUIRE(metadata.HasValue());
            Write("ui-paint-metal.horoshdr", kernel.artifact.payload);
            Write("ui-paint-metal.reflection.json", metadata.Value());
        }
    };
}  // namespace Horo::Tests::UiPaintPackageTesting
