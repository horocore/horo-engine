#include "EditorViewportShaderParameters.h"
#include "EditorViewportShaderRecipe.h"

#include <algorithm>
#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Render;

    /** @brief Exact float-register ABI unit layout, not a compiler artifact or native qualification substitute. */
    MaterialBindingLayout UniformLayout(EditorViewportShaderProgram role, bool clustered) {
        ShaderTargetRequirement target{.backend = ShaderTargetBackend::Metal,
                                       .payloadFormat = ShaderPayloadFormat::MetalLibrary24,
                                       .descriptorVersion = 1,
                                       .interfaceSchemaVersion = 1,
                                       .maximumBindings = 32,
                                       .maximumInlineConstantBytes = 128,
                                       .supportsCompute = true,
                                       .supportsStorageResources = true};
        const auto manifest =
            clustered ? MakeEditorViewportClusteredShaderManifest(role, target) : MakeEditorViewportShaderManifest(role, target);
        REQUIRE(manifest.HasValue());
        MaterialBindingLayout layout;
        layout.backend = target.backend;
        layout.resources = {{{1},
                             ShaderResourceKind::UniformBuffer,
                             ShaderResourceAccess::ReadOnly,
                             1,
                             ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment,
                             true}};
        layout.targetBindings = {{{1}, 0, 0, 0, "Parameters", {}, true}};
        std::uint32_t offset = 0;
        for (const auto &parameter : manifest.Value().parameters) {
            if (parameter.binding != ShaderBindingId{1})
                continue;
            const bool matrix = parameter.rows == 4;
            const std::uint32_t elementBytes = matrix ? 64 : 16;
            layout.parameters.push_back({parameter.id, parameter.binding, parameter.type, parameter.rows, parameter.columns,
                                         parameter.arrayCount, offset, parameter.arrayCount > 1 ? elementBytes : 0, matrix ? 16U : 0U, true,
                                         true});
            offset += elementBytes * parameter.arrayCount;
        }
        return layout;
    }

    Math::Mat4 NonsymmetricMatrix(float seed) {
        return Math::Mat4{{seed + 1, seed + 2, seed + 3, 0, seed + 5, seed + 6, seed + 7, 0, seed + 9, seed + 10, seed + 11, 0, seed + 13,
                           seed + 14, seed + 15, 1}};
    }

    void RequireNativeMatrix(std::span<const std::byte> bytes, std::size_t offset, const Math::Mat4 &matrix) {
        REQUIRE(offset + sizeof(matrix.values) <= bytes.size());
        // Independent byte oracle: Metal float4x4 columns must match the original scene matrix storage.
        const auto expected = std::as_bytes(std::span{matrix.values});
        CHECK(std::ranges::equal(bytes.subspan(offset, expected.size()), expected));
        std::array<std::byte, 4> translation;
        std::ranges::copy(bytes.subspan(offset + 48, 4), translation.begin());
        CHECK(std::bit_cast<float>(translation) == matrix.values[12]);
    }
}  // namespace

TEST_CASE("Viewport lit and clustered material writers preserve native matrix columns and every cascade", "[editor][matrix-packing]") {
    const bool clustered = GENERATE(false, true);
    const auto layout = UniformLayout(EditorViewportShaderProgram::Lit, clustered);
    EditorViewportShaderParameters source;
    source.mvp = NonsymmetricMatrix(1);
    source.model = NonsymmetricMatrix(20);
    std::array<Math::Mat4, 4> cascades;
    for (std::size_t cascade = 0; cascade < cascades.size(); ++cascade) {
        cascades[cascade] = NonsymmetricMatrix(100 + static_cast<float>(cascade) * 20);
        std::ranges::copy(cascades[cascade].values, source.shadow.begin() + cascade * 16);
    }
    std::array<std::byte, 2048> bytes;
    Result<std::size_t> written = Result<std::size_t>::Success(0);
    if (clustered) {
        EditorViewportClusteredShaderParameters parameters;
        parameters.common = source;
        written = parameters.PackLit(layout, bytes);
    } else {
        written = source.PackLit(layout, bytes);
    }
    REQUIRE(written.HasValue());
    CHECK(written.Value() == (clustered ? 704 : 1664));
    RequireNativeMatrix(bytes, 0, source.mvp);
    RequireNativeMatrix(bytes, 64, source.model);
    for (std::size_t cascade = 0; cascade < cascades.size(); ++cascade)
        RequireNativeMatrix(bytes, 128 + cascade * 64, cascades[cascade]);
}

TEST_CASE("Viewport grid and shadow material writers preserve nonidentity model projection bytes", "[editor][matrix-packing]") {
    const auto matrix = NonsymmetricMatrix(8);
    const auto grid = UniformLayout(EditorViewportShaderProgram::Grid, false);
    const auto shadow = UniformLayout(EditorViewportShaderProgram::Shadow, false);
    std::array<std::byte, 80> gridBytes;
    std::array<std::byte, 64> shadowBytes;
    const auto gridWritten = PackEditorViewportGrid(grid, matrix, {0.25F, 0.5F, 0.75F}, gridBytes);
    const auto shadowWritten = PackEditorViewportShadow(shadow, matrix, shadowBytes);
    REQUIRE(gridWritten.HasValue());
    REQUIRE(shadowWritten.HasValue());
    CHECK(gridWritten.Value() == 80);
    CHECK(shadowWritten.Value() == 64);
    RequireNativeMatrix(gridBytes, 0, matrix);
    RequireNativeMatrix(shadowBytes, 0, matrix);
    const std::array<float, 4> color{0.25F, 0.5F, 0.75F, 1};
    CHECK(std::ranges::equal(std::span{gridBytes}.subspan(64), std::as_bytes(std::span{color})));
}
