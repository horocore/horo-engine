#include "MetalUiPaintFixture.h"

#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <sstream>

namespace Horo::Tests {
    namespace {
        /** @brief Reads bounded regular qualification data without following symlink components. */
        std::vector<std::uint8_t> Read(const std::filesystem::path &path, const std::size_t maximum) {
            for (auto part = path; !part.empty();) {
                REQUIRE_FALSE(std::filesystem::is_symlink(part));
                const auto parent = part.parent_path();
                if (parent == part)
                    break;
                part = parent;
            }
            REQUIRE(std::filesystem::is_regular_file(path));
            const auto size = std::filesystem::file_size(path);
            REQUIRE(size > 0);
            REQUIRE(size <= maximum);
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            std::ifstream input(path, std::ios::binary);
            input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(input.good());
            return bytes;
        }

        /** @brief Checks actual linked bytes against the qualification receipt before packaging. */
        Render::ShaderArtifactStage Library(const std::filesystem::path &root, const Render::ShaderStage stage, const std::string &entry) {
            auto bytes = Read(root / (entry + ".metallib"), 8U * 1024U * 1024U);
            const auto expected = Read(root / (entry + ".sha256"), 72);
            const std::string digest(expected.begin(), expected.end());
            REQUIRE(digest == FormatSha256(ComputeSha256(std::as_bytes(std::span{bytes}))) + "\n");
            REQUIRE(bytes.size() >= 4);
            REQUIRE((std::array{bytes[0], bytes[1], bytes[2], bytes[3]} == std::array<std::uint8_t, 4>{'M', 'T', 'L', 'B'}));
            return {stage, entry, std::move(bytes)};
        }

        /** @brief Loads only the finite map derived from generated MSL, never arbitrary caller-selected bindings. */
        std::array<std::uint32_t, 4> Bindings(const std::filesystem::path &root) {
            const auto bytes = Read(root / "bindings.txt", 64);
            std::istringstream input(std::string(bytes.begin(), bytes.end()));
            std::array<std::uint32_t, 4> bindings{};
            for (auto &binding : bindings) {
                REQUIRE(static_cast<bool>(input >> binding));
                REQUIRE(binding <= 30);
            }
            input >> std::ws;
            REQUIRE(input.eof());
            return bindings;
        }
    }  // namespace

    /** @copydoc LoadMetalUiPaintFixture */
    Render::CookedUiPaintKernel LoadMetalUiPaintFixture(const std::filesystem::path &root, const Render::RenderTextureFormat format) {
        using namespace Render;
        CookedUiPaintKernel kernel;
        kernel.target = {ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24, 1, 1, 4, 0, false, false};
        kernel.colorFormat = format;
        kernel.stencilFormat = RenderTextureFormat::Depth32FloatStencil8;
        kernel.manifest = MakeUiPaintShaderManifest(kernel.target);
        const std::array stages{Library(root, ShaderStage::Vertex, "PaintVertex"), Library(root, ShaderStage::Fragment, "PaintFragment")};
        auto package = PackageShaderArtifactStages(kernel.target.backend, kernel.target.payloadFormat, stages, 16U * 1024U * 1024U);
        REQUIRE(package.HasValue());
        kernel.artifact.backend = kernel.target.backend;
        kernel.artifact.payloadFormat = kernel.target.payloadFormat;
        kernel.artifact.payload = std::move(package).Value();
        kernel.artifact.artifactKey = ComputeSha256(std::as_bytes(std::span{kernel.artifact.payload}));
        const auto bindings = Bindings(root);
        kernel.reflection.backend = kernel.target.backend;
        kernel.reflection.interfaceSchemaVersion = 1;
        for (const auto &binding : kernel.manifest.bindings) {
            kernel.reflection.bindings.push_back({binding.id, binding.kind, binding.access, 1, binding.stages, true});
            kernel.reflection.targetBindings.push_back({.id = binding.id, .nativeBinding = bindings[binding.id.value - 1]});
        }
        REQUIRE(ValidateCookedUiPaintKernel(kernel, ShaderTargetBackend::Metal).HasValue());
        return kernel;
    }
}  // namespace Horo::Tests
