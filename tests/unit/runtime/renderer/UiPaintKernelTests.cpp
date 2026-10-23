#include "Horo/Runtime/Render/UiPaintKernel.h"
#include "Horo/Runtime/Render/UiPaintWorkload.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string_view>

namespace Horo::Render {
    namespace {
        template <typename T> void Append(std::vector<std::uint8_t> &bytes, const T value) {
            for (std::size_t index = 0; index < sizeof(T); ++index)
                bytes.push_back(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
        }

        void AppendEntry(std::vector<std::uint8_t> &bytes, const ShaderStage stage, const std::string_view name) {
            Append(bytes, static_cast<std::uint8_t>(stage));
            Append(bytes, static_cast<std::uint32_t>(name.size()));
            bytes.insert(bytes.end(), name.begin(), name.end());
            Append(bytes, std::uint64_t{4});
            bytes.insert(bytes.end(), {1, 2, 3, 4});
        }

        /** @brief Metadata fixture proves package/interface rejection only; its bytes are deliberately not native shader code. */
        CookedUiPaintKernel Kernel() {
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
            kernel.reflection.backend = kernel.target.backend;
            kernel.reflection.interfaceSchemaVersion = 1;
            for (const auto &binding : kernel.manifest.bindings) {
                kernel.reflection.bindings.push_back({binding.id, binding.kind, binding.access, 1, binding.stages, true});
                kernel.reflection.targetBindings.push_back({.id = binding.id, .nativeBinding = binding.id.value - 1});
            }
            return kernel;
        }
    }  // namespace

    TEST_CASE("UI paint metadata validates exact two-stage owned shader interface", "[runtime][renderer][ui-paint]") {
        const auto kernel = Kernel();
        REQUIRE(ValidateCookedUiPaintKernel(kernel, ShaderTargetBackend::Metal).HasValue());
        REQUIRE(ValidateCookedUiPaintKernel(kernel, ShaderTargetBackend::Vulkan).HasError());
        const auto vertex = UiPaintNativePayload(kernel.artifact, ShaderStage::Vertex);
        const auto fragment = UiPaintNativePayload(kernel.artifact, ShaderStage::Fragment);
        REQUIRE(vertex.HasValue());
        REQUIRE(fragment.HasValue());
        REQUIRE(vertex.Value().size() == 4);
        REQUIRE(fragment.Value().data() != vertex.Value().data());
    }

    TEST_CASE("UI paint shader admission rejects malformed package and unsupported interface", "[runtime][renderer][ui-paint]") {
        auto kernel = Kernel();
        SECTION("Incomplete package header") {
            kernel.artifact.payload.resize(17);
        }
        SECTION("Oversized package extent") {
            kernel.artifact.payload.resize(16U * 1024U * 1024U + 1);
        }
        SECTION("Trailing bytes") {
            kernel.artifact.payload.push_back(0);
        }
        SECTION("Truncated final record") {
            kernel.artifact.payload.pop_back();
        }
        SECTION("Wrong stage order") {
            kernel.artifact.payload[18] = static_cast<std::uint8_t>(ShaderStage::Fragment);
        }
        SECTION("Duplicate entry") {
            kernel.manifest.entryPoints[1] = kernel.manifest.entryPoints[0];
        }
        SECTION("Wrong binding role") {
            kernel.manifest.bindings[1].kind = ShaderResourceKind::StorageTexture;
        }
        SECTION("Missing active source") {
            kernel.reflection.bindings[1].active = false;
        }
        SECTION("No stencil format") {
            kernel.stencilFormat = RenderTextureFormat::Depth32Float;
        }
        SECTION("Wrong payload target") {
            kernel.artifact.backend = ShaderTargetBackend::Vulkan;
        }
        SECTION("Missing key") {
            kernel.artifact.artifactKey = {};
        }
        REQUIRE(ValidateCookedUiPaintKernel(kernel, ShaderTargetBackend::Metal).HasError());
    }

    TEST_CASE("UI paint scissors preserve empty clipping and reject overflow", "[runtime][renderer][ui-paint]") {
        REQUIRE((UiPaintScissor{0, 0, 640, 480}.Fits({640, 480})));
        REQUIRE((UiPaintScissor{640, 480, 0, 0}.Fits({640, 480})));
        REQUIRE_FALSE((UiPaintScissor{641, 0, 0, 0}.Fits({640, 480})));
        REQUIRE_FALSE((UiPaintScissor{1, 0, 0xffffffffU, 1}.Fits({640, 480})));
    }
}  // namespace Horo::Render
