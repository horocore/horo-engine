#include "MetalUiPaintFixture.h"

#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "ShaderReflectionCook.h"

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
        std::array<SpirvReflectionStage, 2> reflected;
        std::array<std::string, 2> translations;
        for (std::size_t index = 0; index < stages.size(); ++index) {
            const auto &entry = stages[index].entryPoint;
            const auto json = Read(root / (entry + ".reflection.json"), 1024U * 1024U);
            const auto msl = Read(root / (entry + ".metal"), 1024U * 1024U);
            reflected[index] = {stages[index].stage, entry, {json.begin(), json.end()}};
            translations[index] = {msl.begin(), msl.end()};
        }
        auto normalized = NormalizeUiPaintMetalReflection(kernel.manifest, kernel.target, reflected, translations);
        REQUIRE(normalized.HasValue());
        const auto identity = Read(root / "reflection-tool.txt", 328);
        std::istringstream input(std::string{identity.begin(), identity.end()});
        std::string release, digest;
        REQUIRE(static_cast<bool>(std::getline(input, release)));
        REQUIRE(static_cast<bool>(std::getline(input, digest)));
        REQUIRE(input.peek() == std::char_traits<char>::eof());
        auto toolDigest = ParseSha256(digest);
        REQUIRE(toolDigest.HasValue());
        auto record = CookReflectedShaderArtifact(kernel.artifact, {ShaderCompilerTool::SpirvCross, release, toolDigest.Value()},
                                                  std::move(normalized).Value());
        REQUIRE(record.HasValue());
        kernel.artifact.reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(record).Value());
        REQUIRE(ValidateCookedUiPaintKernel(kernel, ShaderTargetBackend::Metal).HasValue());
        return kernel;
    }
}  // namespace Horo::Tests
