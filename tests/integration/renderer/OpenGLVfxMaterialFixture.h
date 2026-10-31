/** @file
 * @brief Native VFX smoke material setup through the canonical captured-shader, pipeline and resident binding owners.
 */
#pragma once

#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "LockedVfxShaderFixture.h"
#include "OpenGLMaterialFixture.h"

namespace Horo::Render::OpenGLMaterialSmoke::VfxFixture {
    using namespace OpenGLMaterialTests;

    struct MaterialFixture {
        MaterialRuntimeId id;
        PipelineFixture pipeline;
        RenderBufferHandle instances;
        TextureFixture texture;
        RenderSamplerHandle sampler;
        std::unique_ptr<MaterialBindingTable> table;
        std::shared_ptr<PreparedVfxFrameSlot> slot;
    };

    /** @brief Prepares one exact packaged captured stage and preserves the native Result diagnostic. */
    inline RenderShaderModuleHandle CapturedStage(RenderFrontend &frontend, const Tests::LockedVfxShaderEvidence &evidence,
                                                  const CompiledShaderArtifact &artifact, const ShaderStage stage, const char *entry) {
        REQUIRE(artifact.reflection);
        const auto prepared = frontend.CreateShaderModule({evidence.target, stage, entry, artifact.artifactKey,
                                                           artifact.reflection->reflection.interfaceCompatibility},
                                                          artifact.payload);
        INFO((prepared.HasError() ? prepared.ErrorValue().message : "Exact captured GL stage prepared"));
        REQUIRE(prepared.HasValue());
        return prepared.Value().handle;
    }

    /** @brief Creates the real linked VFX pipeline for one authored blend/depth mode. */
    inline PipelineFixture Pipeline(RenderFrontend &frontend, const std::uint32_t index) {
        const bool masked = index == 3;
        const auto evidence = Tests::LoadLockedVfxShaderEvidence(ShaderTargetBackend::OpenGL, masked);
        const auto artifact = Tests::CookLockedVfxOpenGlFixture(evidence);
        REQUIRE(artifact.reflection);
        const auto &reflection = artifact.reflection->reflection;
        const auto vertex = CapturedStage(frontend, evidence, artifact, ShaderStage::Vertex, "VSMain");
        const auto fragment = CapturedStage(frontend, evidence, artifact, ShaderStage::Fragment, "PSMain");
        RenderGraphicsPipelineState state;
        state.colorFormat = RenderTextureFormat::Rgba16Float;
        state.depthFormat = RenderTextureFormat::Depth24Stencil8;
        state.cull = RenderCullMode::None;
        state.depthTest = true;
        state.depthWrite = index != 2 && index != 4;
        state.blend = index == 2 ? RenderBlendMode::Alpha : index == 4 ? RenderBlendMode::Additive : RenderBlendMode::Disabled;
        auto description =
            PipelineDescriptor(frontend, vertex, fragment, evidence.manifest, evidence.target, reflection, artifact.artifactKey, state);
        const auto native = frontend.CreateGraphicsPipeline(description);
        INFO((native.HasError() ? native.ErrorValue().message : "Actual cooked VFX GL pipeline admitted"));
        REQUIRE(native.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        auto layout = PrepareMaterialBindingLayout(reflection);
        INFO("layout: " << (layout.HasError() ? layout.ErrorValue().domain.Value() : "") << '/'
                        << (layout.HasError() ? layout.ErrorValue().code.Value() : "") << ": "
                        << (layout.HasError() ? layout.ErrorValue().message : "success"));
        REQUIRE(layout.HasValue());
        return {vertex, fragment, native.Value().handle, std::move(layout).Value(), std::move(description)};
    }

    /** @brief Creates the white/masked texel or nonuniform UV sentinel atlas used by this scene. */
    inline TextureFixture Texture(RenderFrontend &frontend, const std::uint32_t index) {
        const std::array<std::byte, 4> texel{std::byte{255}, std::byte{255}, std::byte{255}, (index == 3) ? std::byte{0} : std::byte{255}};
        const std::array<std::byte, 16> atlas{std::byte{32},  std::byte{32},  std::byte{32},  std::byte{255},
                                              std::byte{128}, std::byte{128}, std::byte{128}, std::byte{255},
                                              std::byte{64},  std::byte{64},  std::byte{64},  std::byte{255},
                                              std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
        RenderTextureDescriptor textureDescription{.extent = {1, 1},
                                                   .format = RenderTextureFormat::Rgba8Unorm,
                                                   .usage = RenderTextureUsage::Sampled};
        std::span<const std::byte> samples{texel};
        if (index == 1) {
            textureDescription.extent = {2, 2};
            samples = atlas;
        }
        return PrepareTexture(frontend, textureDescription, RenderTextureAspect::Color, samples);
    }

    /** @brief Packs the scene's immutable tint and alpha threshold using canonical reflection. */
    inline MaterialParameterBinding Constants(const MaterialBindingLayout &layout) {
        const std::array<float, 4> tint{1, 1, 1, 1};
        const std::array<float, 1> cutoff{0.5F};
        const std::array parameters{MaterialParameterValue{VfxShaderParameterIds::Tint, std::span<const float>{tint}},
                                    MaterialParameterValue{VfxShaderParameterIds::AlphaCutoff, std::span<const float>{cutoff}}};
        auto constants = PackMaterialParameterBinding(layout, VfxShaderParameterIds::Material, parameters);
        INFO("constants: " << (constants.HasError() ? constants.ErrorValue().domain.Value() : "") << '/'
                           << (constants.HasError() ? constants.ErrorValue().code.Value() : "") << ": "
                           << (constants.HasError() ? constants.ErrorValue().message : "success"));
        REQUIRE(constants.HasValue());
        return std::move(constants).Value();
    }

    /** @brief Publishes one exact material generation and acquires its uniquely owned upload slot. */
    inline void PrepareSlot(RenderFrontend &frontend, MaterialFixture &fixture, const std::size_t byteCount) {
        auto table = MaterialBindingTable::Create(fixture.pipeline.pipeline.owner, frontend);
        INFO("table: " << (table.HasError() ? table.ErrorValue().domain.Value() : "") << '/'
                       << (table.HasError() ? table.ErrorValue().code.Value() : "") << ": "
                       << (table.HasError() ? table.ErrorValue().message : "success"));
        REQUIRE(table.HasValue());
        fixture.table = std::move(table).Value();
        const auto published = fixture.table->Publish(
            {.material = fixture.id,
             .revision = 1,
             .pipeline = fixture.pipeline.pipeline,
             .layout = fixture.pipeline.layout,
             .resources = {{VfxShaderParameterIds::Instances, 0, MaterialBufferBinding{fixture.instances, 0, byteCount}},
                           {VfxShaderParameterIds::Material, 0, Constants(fixture.pipeline.layout)},
                           {VfxShaderParameterIds::BaseColor, 0, fixture.texture.view},
                           {VfxShaderParameterIds::BaseColorSampler, 0, fixture.sampler}}});
        INFO("published: " << (published.HasError() ? published.ErrorValue().domain.Value() : "") << '/'
                           << (published.HasError() ? published.ErrorValue().code.Value() : "") << ": "
                           << (published.HasError() ? published.ErrorValue().message : "success"));
        REQUIRE(published.HasValue());
        auto resident = fixture.table->Acquire(published.Value().binding);
        INFO("resident: " << (resident.HasError() ? resident.ErrorValue().domain.Value() : "") << '/'
                          << (resident.HasError() ? resident.ErrorValue().code.Value() : "") << ": "
                          << (resident.HasError() ? resident.ErrorValue().message : "success"));
        REQUIRE(resident.HasValue());
        auto slot = frontend.PrepareVfxFrameSlot(std::move(resident).Value());
        // Slot preparation checks the actual linked instance block; immutable Material constants are native-size-padded by the
        // frontend.
        INFO("slot: " << (slot.HasError() ? slot.ErrorValue().domain.Value() : "") << '/'
                      << (slot.HasError() ? slot.ErrorValue().code.Value() : "") << ": "
                      << (slot.HasError() ? slot.ErrorValue().message : "success"));
        REQUIRE(slot.HasValue());
        fixture.slot = std::move(slot).Value();
    }

    /** @brief Owns the bounded pipeline, sampled resources and host-visible instance slot for a scene material. */
    inline MaterialFixture Material(RenderFrontend &frontend, const std::uint32_t index) {
        MaterialFixture fixture{.id = {100 + index}, .pipeline = Pipeline(frontend, index)};
        const auto bytes = VfxShaderParameterBytes(fixture.pipeline.layout);
        INFO("bytes: " << (bytes.HasError() ? bytes.ErrorValue().domain.Value() : "") << '/'
                       << (bytes.HasError() ? bytes.ErrorValue().code.Value() : "") << ": "
                       << (bytes.HasError() ? bytes.ErrorValue().message : "success"));
        REQUIRE(bytes.HasValue());
        const auto buffer = frontend.CreateBuffer({.byteSize = bytes.Value(),
                                                   .usage = RenderBufferUsage::Uniform,
                                                   .access = RenderBufferAccess::HostVisible},
                                                  {});
        INFO("buffer: " << (buffer.HasError() ? buffer.ErrorValue().domain.Value() : "") << '/'
                        << (buffer.HasError() ? buffer.ErrorValue().code.Value() : "") << ": "
                        << (buffer.HasError() ? buffer.ErrorValue().message : "success"));
        REQUIRE(buffer.HasValue());
        fixture.instances = buffer.Value().handle;
        fixture.texture = Texture(frontend, index);
        const auto sampler = frontend.CreateSampler({});
        INFO("sampler: " << (sampler.HasError() ? sampler.ErrorValue().domain.Value() : "") << '/'
                         << (sampler.HasError() ? sampler.ErrorValue().code.Value() : "") << ": "
                         << (sampler.HasError() ? sampler.ErrorValue().message : "success"));
        REQUIRE(sampler.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        fixture.sampler = sampler.Value().handle;
        PrepareSlot(frontend, fixture, bytes.Value());
        return fixture;
    }
}  // namespace Horo::Render::OpenGLMaterialSmoke::VfxFixture
