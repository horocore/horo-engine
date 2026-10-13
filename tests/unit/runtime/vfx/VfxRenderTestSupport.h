#pragma once

#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Vfx/VfxWorld.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace Test::VfxRender {
    using Horo::Render::MaterialAlphaMode;
    using Horo::Render::RegisterNullRenderBackend;
    using Horo::Render::RenderBackendId;
    using Horo::Render::RenderBackendRegistry;
    using Horo::Render::RenderFrontend;
    using Horo::Render::RenderGraphBuilder;
    using Horo::Render::RenderGraphDependencyKind;
    using Horo::Render::RenderGraphResourceClass;
    using Horo::Render::RenderGraphResourceId;
    using Horo::Render::RenderPassKind;
    using Horo::Render::RenderQueueRole;
    using Horo::Render::RenderResourceState;
    using Horo::Render::RenderTextureFormat;
    using Horo::Render::RenderTextureUsage;
    using Horo::Render::VfxGraphRequest;
    using Horo::Render::VfxParticlePass;
    using Horo::Render::VfxPrimitiveTopology;
    using Horo::Vfx::ParticleSimulationId;
    using Horo::Vfx::VfxEmitterPreparation;
    using Horo::Vfx::VfxExtractionBudget;
    using Horo::Vfx::VfxIdentityScope;
    using Horo::Vfx::VfxWorld;

    inline VfxIdentityScope Scope() {
        return VfxIdentityScope::Create(17).Value();
    }

    inline VfxEmitterPreparation Binding(std::uint32_t slot, VfxParticlePass blend = VfxParticlePass::Additive) {
        const bool solid = blend == VfxParticlePass::Opaque || blend == VfxParticlePass::Masked;
        return {.effect = {Scope(), slot, 1},
                .emitter = {Scope(), slot, 1},
                .particles = {.buffer = {Scope(), slot, 1}, .capacity = 16},
                .material = {slot + 1},
                .materialBlend = static_cast<MaterialAlphaMode>(blend),
                .mesh = {{41}, slot + 1, 1},
                .blend = blend,
                .topology = solid ? VfxPrimitiveTopology::InstancedMesh : VfxPrimitiveTopology::CameraFacingBillboard,
                .deferredCompatible = solid,
                .shadowCaster = solid};
    }

    inline VfxExtractionBudget Budget() {
        return {16, 64, 128, 2, 1'000'000};
    }

    inline std::unique_ptr<VfxWorld> World(std::span<const VfxEmitterPreparation> bindings, VfxExtractionBudget budget = Budget()) {
        auto world = VfxWorld::Prepare(Scope(), bindings, budget);
        REQUIRE(world.HasValue());
        return std::move(world).Value();
    }

    inline void Populate(VfxWorld &world, const VfxEmitterPreparation &binding, std::uint32_t count = 2) {
        REQUIRE(world.Play(binding.effect).HasValue());
        auto particles = world.Particles(binding.effect, binding.emitter);
        REQUIRE(particles.HasValue());
        for (std::uint32_t index = 0; index < count; ++index)
            REQUIRE(particles.Value()->Spawn(ParticleSimulationId::Create(index + 1).Value()).HasValue());
        auto view = particles.Value()->View();
        REQUIRE(view.HasValue());
        for (std::uint32_t index = 0; index < count; ++index) {
            view.Value().positionZ[index] = -static_cast<float>(index + 1);
            view.Value().sizeX[index] = 1;
            view.Value().sizeY[index] = 1;
            view.Value().packedColor[index] = 0x11223344U;
        }
    }

    inline std::unique_ptr<RenderFrontend> Frontend() {
        RenderBackendRegistry registry;
        REQUIRE(RegisterNullRenderBackend(registry).HasValue());
        REQUIRE(registry.Seal().HasValue());
        auto frontend = RenderFrontend::Create(registry, RenderBackendId{"null"}, {});
        REQUIRE(frontend.HasValue());
        REQUIRE(frontend.Value()->PrepareVfxScheduling({64, 8, 1'000'000}).HasValue());
        return std::move(frontend).Value();
    }

    inline RenderGraphResourceId Texture(RenderFrontend &frontend, RenderGraphBuilder &builder, RenderTextureFormat format) {
        auto texture = frontend.CreateTexture({.extent = {16, 16}, .format = format, .usage = RenderTextureUsage::RenderAttachment});
        REQUIRE(texture.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        REQUIRE(frontend.ResourceState(texture.Value().handle).Value() == RenderResourceState::Ready);
        auto imported = builder.ImportTexture(texture.Value().handle, RenderGraphResourceClass::Persistent);
        REQUIRE(imported.HasValue());
        return imported.Value();
    }

    inline RenderGraphBuilder Builder(std::size_t maximumPasses = 32) {
        auto builder = RenderGraphBuilder::Create({.maxPasses = maximumPasses, .maxResources = 8, .maxUsages = 64, .maxDependencies = 64});
        REQUIRE(builder.HasValue());
        return std::move(builder).Value();
    }

    inline VfxGraphRequest Request(RenderFrontend &frontend, RenderGraphBuilder &builder) {
        VfxGraphRequest request{.depthPrepass = true,
                                .scene = 17,
                                .frame = 1,
                                .originRevision = 1,
                                .view = 1,
                                .viewGeneration = 1,
                                .maximumInstances = 64,
                                .remainingDrawCalls = 128};
        request.opaqueBegin = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics).Value();
        request.opaqueComplete = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics).Value();
        request.outputTransform = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics).Value();
        REQUIRE(builder.AddDependency({request.opaqueBegin, request.opaqueComplete}).HasValue());
        REQUIRE(builder.AddDependency({request.opaqueComplete, request.outputTransform}).HasValue());
        request.sceneColor = Texture(frontend, builder, RenderTextureFormat::Rgba16Float);
        request.sceneDepth = Texture(frontend, builder, RenderTextureFormat::Depth32Float);
        request.shadowDepth = Texture(frontend, builder, RenderTextureFormat::Depth32Float);
        request.gBuffer = Texture(frontend, builder, RenderTextureFormat::Rgba16Float);
        return request;
    }
}  // namespace Test::VfxRender
