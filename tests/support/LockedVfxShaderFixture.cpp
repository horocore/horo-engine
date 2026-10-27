#include "LockedVfxShaderFixture.h"

#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace Horo::Tests {
    namespace {
        /** @brief Reads only fixed captured text, rejecting changed bytes and symbolic-link path components. */
        std::string Text(const std::string &name) {
            constexpr std::array<std::pair<std::string_view, std::string_view>, 9> digests{{
                {"masked-ps.glsl", "1efae763df70f44d9d9e57ec594efdb47cf76840ee470b634910a86b44550f02"},
                {"masked-ps.metal", "9a8dfb004bf6c7280ecb96f50d315ce41183fc374a957bdddba60fb61d829e2a"},
                {"masked-ps.reflection.json", "dce4c958bdb014913103f843ca0aa876aa3ab0a9100d4dbfce208830b266ff82"},
                {"ps.glsl", "016ee1f33a543cc470f24d72f4795d0c4a4f8fdbb9679457b8f1a81947077914"},
                {"ps.metal", "8c12bce2fd804848858d3f36e201fef482c19a1e7e257b9e295de1895bfb0f8d"},
                {"ps.reflection.json", "57ad3f41df81c62708fdf260bf554017d15ef31eb31885b6d0f764ff2d434484"},
                {"vs.glsl", "843435e98e4873e7a276de9975550bd4b60e46b0aca94ed4020b7b96a9dc3867"},
                {"vs.metal", "9a7bea954f7798c597e1b7714a9302483b7e15a5227352ec98e8fbef90c31a27"},
                {"vs.reflection.json", "e011b06799687501ee71cfa4f928f2e0120b8317e7f787715880dc6774ac5517"},
            }};
            const auto digest = std::ranges::find_if(digests, [&](const auto &item) {
                return item.first == name;
            });
            REQUIRE(digest != digests.end());
            const auto path = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "tests/fixtures/shaders/vfx_locked" / name;
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
            REQUIRE(size <= 64U * 1024U);
            std::string text(static_cast<std::size_t>(size), '\0');
            std::ifstream input(path, std::ios::binary);
            input.read(text.data(), static_cast<std::streamsize>(text.size()));
            REQUIRE(input.good());
            const auto expected = ParseSha256("sha256:" + std::string{digest->second});
            REQUIRE(expected.HasValue());
            REQUIRE(ComputeSha256(std::as_bytes(std::span{text})) == expected.Value());
            return text;
        }
    }  // namespace

    /** @copydoc LoadLockedVfxShaderEvidence */
    LockedVfxShaderEvidence LoadLockedVfxShaderEvidence(const Render::ShaderTargetBackend backend, const bool masked) {
        using namespace Render;
        REQUIRE((backend == ShaderTargetBackend::OpenGL || backend == ShaderTargetBackend::Metal));
        const bool metal = backend == ShaderTargetBackend::Metal;
        LockedVfxShaderEvidence evidence;
        evidence.target = {backend, metal ? ShaderPayloadFormat::MetalLibrary24 : ShaderPayloadFormat::Glsl410, 1, 1, 4, 0, false, false};
        evidence.manifest = MakeVfxShaderManifest(evidence.target);
        const std::string fragment = masked ? "masked-ps" : "ps";
        evidence.stages = {{{ShaderStage::Vertex, "VSMain", Text("vs.reflection.json")},
                            {ShaderStage::Fragment, "PSMain", Text(fragment + ".reflection.json")}}};
        const std::string extension = metal ? ".metal" : ".glsl";
        evidence.translations = {Text("vs" + extension), Text(fragment + extension)};
        return evidence;
    }

    /** @copydoc CookLockedVfxOpenGlFixture */
    Render::CompiledShaderArtifact CookLockedVfxOpenGlFixture(const LockedVfxShaderEvidence &evidence) {
        using namespace Render;
        REQUIRE(evidence.target.backend == ShaderTargetBackend::OpenGL);
        auto normalized = NormalizeVfxOpenGLReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations);
        INFO((normalized.HasError() ? normalized.ErrorValue().message : "Actual captured GL reflection admitted"));
        REQUIRE(normalized.HasValue());
        std::array<ShaderArtifactStage, 2> stages;
        for (std::size_t index = 0; index < stages.size(); ++index)
            stages[index] = {evidence.stages[index].stage,
                             evidence.stages[index].entryPoint,
                             {evidence.translations[index].begin(), evidence.translations[index].end()}};
        auto package = PackageShaderArtifactStages(evidence.target.backend, evidence.target.payloadFormat, stages, 128U * 1024U);
        REQUIRE(package.HasValue());
        CompiledShaderArtifact artifact;
        artifact.backend = evidence.target.backend;
        artifact.payloadFormat = evidence.target.payloadFormat;
        artifact.payload = std::move(package).Value();
        artifact.artifactKey = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
        auto digest = ParseSha256("sha256:335caee5ce86daefc3dee5e13100c2118a1a1cccb183a8c6df817090e0cbb976");
        REQUIRE(digest.HasValue());
        auto cooked =
            CookReflectedShaderArtifact(artifact, {ShaderCompilerTool::SpirvCross, "ubuntu-2021.01.15+1.4.335.0-1", digest.Value()},
                                        std::move(normalized).Value());
        REQUIRE(cooked.HasValue());
        artifact.reflection = std::make_shared<const ReflectedShaderArtifact>(std::move(cooked).Value());
        return artifact;
    }
}  // namespace Horo::Tests
