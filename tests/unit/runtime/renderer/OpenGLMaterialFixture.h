#pragma once

#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Render::OpenGLMaterialTests {
    /** @brief Explicit GLSL fixture shared by deterministic ownership tests and separately opted-in real-device tests. */
    struct PipelineFixture {
        RenderShaderModuleHandle vertex;
        RenderShaderModuleHandle fragment;
        RenderPipelineHandle pipeline;
        MaterialBindingLayout layout;
        RenderGraphicsPipelineDescriptor descriptor;
    };

    /** @brief Declares the portable material interface independently of native preparation. */
    inline ShaderManifest MaterialManifest(const ShaderTargetRequirement &target) {
        return {.schemaVersion = 1,
                .sourceIdentity = "tests/material.hlsl",
                .sourceRevision = 1,
                .entryPoints = {{ShaderStage::Vertex, "VertexMain"}, {ShaderStage::Fragment, "FragmentMain"}},
                .bindings = {{ShaderBindingId{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1,
                              ShaderStageVisibility::Fragment},
                             {ShaderBindingId{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1,
                              ShaderStageVisibility::Fragment},
                             {ShaderBindingId{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1,
                              ShaderStageVisibility::Fragment}},
                .parameters = {{ShaderParameterId{1}, ShaderBindingId{1}, ShaderValueType::Float32, 4, 1, 1}},
                .targets = {target}};
    }

    /** @brief Declares exact final-target names and packing for the material fixture. */
    inline ShaderReflectionCandidate MaterialReflection() {
        return {.backend = ShaderTargetBackend::OpenGL,
                .interfaceSchemaVersion = 1,
                .bindings = {{ShaderBindingId{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1,
                              ShaderStageVisibility::Fragment, true},
                             {ShaderBindingId{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1,
                              ShaderStageVisibility::Fragment, true},
                             {ShaderBindingId{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1,
                              ShaderStageVisibility::Fragment, true}},
                .parameters = {{ShaderParameterId{1}, ShaderBindingId{1}, ShaderValueType::Float32, 4, 1, 1, 0, 0, 0, true, true}},
                .stageInterface = {{ShaderStage::Vertex, ShaderInterfaceDirection::Input, 0, ShaderValueType::Float32, 1, 3, 1, "Position"},
                                   {ShaderStage::Vertex, ShaderInterfaceDirection::Output, 0, ShaderValueType::Float32, 1, 2, 1, "UV"},
                                   {ShaderStage::Fragment, ShaderInterfaceDirection::Input, 0, ShaderValueType::Float32, 1, 2, 1, "UV"},
                                   {ShaderStage::Fragment, ShaderInterfaceDirection::Output, 0, ShaderValueType::Float32, 1, 4, 1,
                                    "Color"}},
                .targetBindings = {{ShaderBindingId{1}, 0, 0, 0, "Material", {}, true},
                                   {ShaderBindingId{2}, 0, 0, 1, "Albedo", ShaderBindingId{3}, true},
                                   {ShaderBindingId{3}, 0, 0, 1, "AlbedoSampler", {}, true}}};
    }

    /** @brief Builds the selected-device cache identity without changing shader ownership. */
    inline RenderGraphicsPipelineDescriptor PipelineDescriptor(RenderFrontend &frontend, RenderShaderModuleHandle vertex,
                                                               RenderShaderModuleHandle fragment, const ShaderManifest &manifest,
                                                               const ShaderTargetRequirement &target,
                                                               const NormalizedShaderReflection &reflection, const Sha256Digest &artifact,
                                                               const RenderGraphicsPipelineState &state = {}) {
        const auto identity = frontend.QueryPipelineDeviceIdentity();
        REQUIRE(identity.HasValue());
        const auto stateDigest = ComputeRenderGraphicsPipelineStateDigest(state);
        REQUIRE(stateDigest.HasValue());
        const auto &device = identity.Value();
        return {vertex,
                fragment,
                manifest,
                target,
                reflection,
                state,
                {.schemaVersion = 1,
                 .backend = device.backend,
                 .backendModuleVersion = device.backendModuleVersion,
                 .adapter = device.adapter,
                 .deviceCompatibilityDigest = device.deviceCompatibilityDigest,
                 .driverIdentity = device.driverIdentity,
                 .driverVersion = device.driverVersion,
                 .shaderBackend = target.backend,
                 .shaderFormat = target.payloadFormat,
                 .shaderArtifactKey = artifact,
                 .shaderInterface = reflection.interfaceCompatibility,
                 .pipelineDescriptorDigest = stateDigest.Value()}};
    }

    /** @brief Prepares exact cooked GLSL and final-target reflection through the real frontend admission transactions. */
    inline PipelineFixture PreparePipelineSources(RenderFrontend &frontend, const ShaderTargetRequirement &target,
                                                  const ShaderManifest &manifest, const ShaderReflectionCandidate &candidate,
                                                  const std::string_view vertexSource, const std::string_view fragmentSource,
                                                  const RenderGraphicsPipelineState &state = {}) {
        const auto reflection = NormalizeShaderReflection(manifest, target, candidate);
        REQUIRE(reflection.HasValue());
        const std::array stages{ShaderArtifactStage{ShaderStage::Vertex, "VertexMain", {vertexSource.begin(), vertexSource.end()}},
                                ShaderArtifactStage{ShaderStage::Fragment, "FragmentMain", {fragmentSource.begin(), fragmentSource.end()}}};
        const auto package = PackageShaderArtifactStages(target.backend, target.payloadFormat, stages, 64U * 1024U * 1024U);
        REQUIRE(package.HasValue());
        const auto artifact = ComputeSha256(std::as_bytes(std::span{package.Value()}));
        const auto vertex =
            frontend.CreateShaderModule({target, ShaderStage::Vertex, "VertexMain", artifact, reflection.Value().interfaceCompatibility},
                                        package.Value());
        REQUIRE(vertex.HasValue());
        const auto fragment = frontend.CreateShaderModule({target, ShaderStage::Fragment, "FragmentMain", artifact,
                                                           reflection.Value().interfaceCompatibility},
                                                          package.Value());
        REQUIRE(fragment.HasValue());
        auto descriptor = PipelineDescriptor(frontend, vertex.Value().handle, fragment.Value().handle, manifest, target, reflection.Value(),
                                             artifact, state);
        const auto pipeline = frontend.CreateGraphicsPipeline(descriptor);
        if (pipeline.HasError()) {
            INFO("Pipeline admission: " << pipeline.ErrorValue().domain.Value() << '/' << pipeline.ErrorValue().code.Value() << ": "
                                        << pipeline.ErrorValue().message);
            REQUIRE(pipeline.HasValue());
        }
        REQUIRE(pipeline.HasValue());
        const auto layout = PrepareMaterialBindingLayout(reflection.Value());
        REQUIRE(layout.HasValue());
        return {vertex.Value().handle, fragment.Value().handle, pipeline.Value().handle, layout.Value(), std::move(descriptor)};
    }

    /** @brief Preserves the existing material fixture through the shared exact source/manifest admission. */
    inline PipelineFixture PreparePipeline(RenderFrontend &frontend) {
        const ShaderTargetRequirement target{.backend = ShaderTargetBackend::OpenGL,
                                             .payloadFormat = ShaderPayloadFormat::Glsl410,
                                             .descriptorVersion = 1,
                                             .interfaceSchemaVersion = 1,
                                             .maximumBindings = 16,
                                             .maximumInlineConstantBytes = 128};
        constexpr std::string_view vertex = "#version 410 core\nlayout(location=0) in vec3 Position;out vec2 UV;void "
                                            "main(){UV=(Position.xy+vec2(1))*0.5;gl_Position=vec4(Position,1);}";
        constexpr std::string_view fragment =
            "#version 410 core\nin vec2 UV;layout(location=0) out vec4 Color;layout(std140) uniform Material{vec4 Tint;};uniform sampler2D "
            "Albedo;void main(){Color=texture(Albedo,UV)*Tint;}";
        return PreparePipelineSources(frontend, target, MaterialManifest(target), MaterialReflection(), vertex, fragment);
    }

    /** @brief Creates a ready indexed fullscreen triangle through admitted generic resource preparation. */
    inline RenderMeshHandle PrepareMesh(RenderFrontend &frontend, const bool fixedRestartHostile = false) {
        std::vector<MeshVertex> vertices(fixedRestartHostile ? 65'536 : 3);
        vertices[0].position = {-1, -1, 0};
        vertices[1].position = {3, -1, 0};
        vertices.back().position = {-1, 3, 0};
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        const std::array<std::uint16_t, 3> hostileIndices{0, 1, 65'535};
        std::span<const std::byte> indexBytes = std::as_bytes(std::span{indices});
        if (fixedRestartHostile)
            indexBytes = std::as_bytes(std::span{hostileIndices});
        const auto vertexBytes = std::as_bytes(std::span{vertices});
        const auto vertex = frontend.CreateBuffer({.byteSize = vertexBytes.size(), .usage = RenderBufferUsage::Vertex}, vertexBytes);
        const auto index = frontend.CreateBuffer({.byteSize = indexBytes.size(), .usage = RenderBufferUsage::Index}, indexBytes);
        REQUIRE(vertex.HasValue());
        REQUIRE(index.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        const auto mesh = frontend.CreateMesh({.vertexBuffer = vertex.Value().handle,
                                               .indexBuffer = index.Value().handle,
                                               .vertexStride = sizeof(MeshVertex),
                                               .vertexCount = static_cast<std::uint32_t>(vertices.size()),
                                               .indexFormat = fixedRestartHostile ? RenderIndexFormat::UInt16 : RenderIndexFormat::UInt32,
                                               .indexCount = 3,
                                               .localBounds = {{-1, -1, 0}, {3, 3, 0}}});
        REQUIRE(mesh.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        REQUIRE(frontend.ReleaseBuffer(vertex.Value().handle).HasValue());
        REQUIRE(frontend.ReleaseBuffer(index.Value().handle).HasValue());
        return mesh.Value().handle;
    }

    struct TextureFixture {
        RenderTextureHandle texture;
        RenderTextureViewHandle view;
    };

    /** @brief Prepares one single-mip typed texture and view; uploads remain optional and exact-sized. */
    inline TextureFixture PrepareTexture(RenderFrontend &frontend, RenderTextureDescriptor descriptor, RenderTextureAspect aspect,
                                         std::span<const std::byte> bytes = {}) {
        const auto texture = frontend.CreateTexture(descriptor, bytes);
        REQUIRE(texture.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        const auto view = frontend.CreateTextureView({.texture = texture.Value().handle, .format = descriptor.format, .aspect = aspect});
        REQUIRE(view.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        return {texture.Value().handle, view.Value().handle};
    }

    struct TargetFixture {
        TextureFixture color, depth;
        RenderTargetHandle target;
    };

    /** @brief Creates a non-aliased color/depth target for the prepared pipeline. */
    inline TargetFixture PrepareTarget(RenderFrontend &frontend) {
        const auto color = PrepareTexture(frontend,
                                          {.extent = {64, 32},
                                           .format = RenderTextureFormat::Rgba8Unorm,
                                           .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled},
                                          RenderTextureAspect::Color);
        const auto depth = PrepareTexture(frontend,
                                          {.extent = {64, 32},
                                           .format = RenderTextureFormat::Depth24Stencil8,
                                           .usage = RenderTextureUsage::RenderAttachment},
                                          RenderTextureAspect::DepthStencil);
        const auto target = frontend.CreateRenderTarget({.colorAttachment = color.view, .depthAttachment = depth.view, .extent = {64, 32}});
        REQUIRE(target.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        return {color, depth, target.Value().handle};
    }

    /** @brief Builds a generic packed-parameter/texture/sampler material without settings or native handles. */
    inline MaterialBindingDescriptor Material(const PipelineFixture &pipeline, RenderTextureViewHandle texture,
                                              RenderSamplerHandle sampler) {
        const std::array<float, 4> color{0.25F, 0.75F, 0.5F, 1.0F};
        const auto bytes = std::as_bytes(std::span{color});
        std::vector<std::byte> tint(bytes.begin(), bytes.end());
        return {.material = MaterialRuntimeId{1},
                .revision = 1,
                .pipeline = pipeline.pipeline,
                .layout = pipeline.layout,
                .resources = {{ShaderBindingId{1}, 0, MaterialParameterBinding{std::move(tint)}},
                              {ShaderBindingId{2}, 0, texture},
                              {ShaderBindingId{3}, 0, sampler}}};
    }
}  // namespace Horo::Render::OpenGLMaterialTests
