#include "../../unit/runtime/renderer/RenderGraphTestUtils.h"
#include "Horo/Runtime/Render/RenderGraphExecution.h"
#include "Horo/Runtime/Render/RenderGraphSynchronization.h"
#include "Horo/Runtime/Render/VfxFrameUpload.h"
#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "Horo/Vfx/VfxWorld.h"
#include "LockedVfxShaderFixture.h"
#include "OpenGLMaterialFixture.h"
#include "OpenGLMaterialSmokeHost.h"
#include "OpenGLVfxMaterialFixture.h"
#include "RenderFrontendResourceAccess.h"

#include <algorithm>
#include <cmath>
#include <glad/gl.h>
#include <iostream>

namespace Horo::Render::OpenGLMaterialSmoke {
    namespace {
        using namespace OpenGLMaterialTests;
        using VfxFixture::Material;
        using VfxFixture::MaterialFixture;
        constexpr Math::Vec3 Eye{0.7F, 0.25F, 0.5F};
        constexpr Math::Vec3 Left{-0.6F, 0, -3};
        constexpr Math::Vec3 Right{0.6F, 0, -3};
        constexpr float AuthoredZ = 0.55F;

        /** @brief Test-only completion barrier runs before fixture/frontend teardown even after a fatal assertion. */
        struct CompleteOnExit {
            ~CompleteOnExit() {
                glFinish();
            }
        };

        /** @brief Exact resident mesh plus its graph-importable canonical vertex/index handles. */
        struct MeshFixture {
            RenderMeshHandle mesh;
            RenderBufferHandle vertices, indices;
        };

        /** @brief Creates the canonical unit quad or a mesh whose authored origin has nonzero Z. */
        MeshFixture Mesh(RenderFrontend &frontend, const float z) {
            std::array<MeshVertex, 4> vertices{{{{-0.5F, -0.5F, z}, {0, 0, 1}, {0, 0}},
                                                {{0.5F, -0.5F, z}, {0, 0, 1}, {1, 0}},
                                                {{0.5F, 0.5F, z}, {0, 0, 1}, {1, 1}},
                                                {{-0.5F, 0.5F, z}, {0, 0, 1}, {0, 1}}}};
            if (z != 0)
                for (auto &vertex : vertices)
                    vertex.uv = {0.75F, 0.25F};  // Normal.xy and swapped/flipped UVs must select different atlas texels.
            constexpr std::array<std::uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
            const auto vertex = frontend.CreateBuffer({.byteSize = sizeof(vertices), .usage = RenderBufferUsage::Vertex},
                                                      std::as_bytes(std::span{vertices}));
            const auto index =
                frontend.CreateBuffer({.byteSize = sizeof(indices), .usage = RenderBufferUsage::Index}, std::as_bytes(std::span{indices}));
            INFO("vertex: " << (vertex.HasError() ? vertex.ErrorValue().domain.Value() : "") << '/'
                            << (vertex.HasError() ? vertex.ErrorValue().code.Value() : "") << ": "
                            << (vertex.HasError() ? vertex.ErrorValue().message : "success"));
            REQUIRE(vertex.HasValue());
            INFO("index: " << (index.HasError() ? index.ErrorValue().domain.Value() : "") << '/'
                           << (index.HasError() ? index.ErrorValue().code.Value() : "") << ": "
                           << (index.HasError() ? index.ErrorValue().message : "success"));
            REQUIRE(index.HasValue());
            REQUIRE(frontend.ProcessResourceRequests().HasValue());
            const auto mesh = frontend.CreateMesh({.vertexBuffer = vertex.Value().handle,
                                                   .indexBuffer = index.Value().handle,
                                                   .vertexStride = sizeof(MeshVertex),
                                                   .vertexCount = 4,
                                                   .indexCount = 6,
                                                   .localBounds = {{-0.5F, -0.5F, z}, {0.5F, 0.5F, z}}});
            INFO("mesh: " << (mesh.HasError() ? mesh.ErrorValue().domain.Value() : "") << '/'
                          << (mesh.HasError() ? mesh.ErrorValue().code.Value() : "") << ": "
                          << (mesh.HasError() ? mesh.ErrorValue().message : "success"));
            REQUIRE(mesh.HasValue());
            REQUIRE(frontend.ProcessResourceRequests().HasValue());
            return {mesh.Value().handle, vertex.Value().handle, index.Value().handle};
        }

        /** @brief Builds an off-axis projection and basis independently of the VFX instance packer. */
        VfxDrawView View() {
            const auto view = Math::TryLookAt(Eye, {0, 0, -3}, {0, 1, 0});
            const auto projection = Math::TryPerspective(1.1F, 2, 0.2F, 80, Math::ClipDepthRange::NegativeOneToOne);
            INFO("view: " << (view.HasError() ? view.ErrorValue().domain.Value() : "") << '/'
                          << (view.HasError() ? view.ErrorValue().code.Value() : "") << ": "
                          << (view.HasError() ? view.ErrorValue().message : "success"));
            REQUIRE(view.HasValue());
            INFO("projection: " << (projection.HasError() ? projection.ErrorValue().domain.Value() : "") << '/'
                                << (projection.HasError() ? projection.ErrorValue().code.Value() : "") << ": "
                                << (projection.HasError() ? projection.ErrorValue().message : "success"));
            REQUIRE(projection.HasValue());
            return {.viewProjection = Math::Multiply(projection.Value(), view.Value()),
                    .right = {view.Value().At(0, 0), view.Value().At(0, 1), view.Value().At(0, 2)},
                    .up = {view.Value().At(1, 0), view.Value().At(1, 1), view.Value().At(1, 2)},
                    .scene = 17,
                    .frame = 1,
                    .originRevision = 1,
                    .view = 2,
                    .viewGeneration = 3};
        }

        /** @brief Moves along the actual eye ray so foreground/background layers cover the same reference pixel. */
        Math::Vec3 AlongRay(const Math::Vec3 point, const float distance) {
            return Eye + (point - Eye) * distance;
        }

        /** @brief Owns canonical simulation, material and mesh resources; all construction is outside measured frame work. */
        struct Scene {
            MeshFixture billboard, mesh;
            TargetFixture target;
            std::vector<MaterialFixture> materials;
            std::unique_ptr<Horo::Vfx::VfxWorld> world;
            VfxDrawView view{View()};

            /** @brief Prepares the real floating color/depth target for the off-axis scene. */
            static TargetFixture Target(RenderFrontend &frontend) {
                const auto color = PrepareTexture(frontend,
                                                  {.extent = {64, 32},
                                                   .format = RenderTextureFormat::Rgba16Float,
                                                   .usage = RenderTextureUsage::RenderAttachment},
                                                  RenderTextureAspect::Color);
                const auto depth = PrepareTexture(frontend,
                                                  {.extent = {64, 32},
                                                   .format = RenderTextureFormat::Depth24Stencil8,
                                                   .usage = RenderTextureUsage::RenderAttachment},
                                                  RenderTextureAspect::DepthStencil);
                const auto prepared =
                    frontend.CreateRenderTarget({.colorAttachment = color.view, .depthAttachment = depth.view, .extent = {64, 32}});
                INFO("prepared: " << (prepared.HasError() ? prepared.ErrorValue().domain.Value() : "") << '/'
                                  << (prepared.HasError() ? prepared.ErrorValue().code.Value() : "") << ": "
                                  << (prepared.HasError() ? prepared.ErrorValue().message : "success"));
                REQUIRE(prepared.HasValue());
                REQUIRE(frontend.ProcessResourceRequests().HasValue());
                return {color, depth, prepared.Value().handle};
            }

            /** @brief Derives typed emitter topology and material roles from the authored scene. */
            std::array<Horo::Vfx::VfxEmitterPreparation, 5> Emitters(const Horo::Vfx::VfxIdentityScope scope) const {
                constexpr std::array passes{VfxParticlePass::Opaque, VfxParticlePass::Opaque, VfxParticlePass::Translucent,
                                            VfxParticlePass::Masked, VfxParticlePass::Additive};
                constexpr std::array blends{MaterialAlphaMode::Opaque, MaterialAlphaMode::Opaque, MaterialAlphaMode::Translucent,
                                            MaterialAlphaMode::Masked, MaterialAlphaMode::Additive};
                std::array<Horo::Vfx::VfxEmitterPreparation, 5> emitters;
                for (std::uint32_t index = 0; index < emitters.size(); ++index)
                    emitters[index] = {.effect = {scope, index, 1},
                                       .emitter = {scope, index, 1},
                                       .particles = {.buffer = {scope, index, 1}, .capacity = 8},
                                       .material = materials[index].id,
                                       .materialBlend = blends[index],
                                       .mesh = index == 1                 ? mesh.mesh
                                               : index == 0 || index == 3 ? billboard.mesh
                                                                          : RenderMeshHandle{},
                                       .blend = passes[index],
                                       .topology = index == 2 || index == 4 ? VfxPrimitiveTopology::CameraFacingBillboard
                                                                            : VfxPrimitiveTopology::InstancedMesh};
                return emitters;
            }

            /** @brief Populates one real emitter with exact position, size, rotation and color sentinels. */
            void PopulateEmitter(const Horo::Vfx::VfxEmitterPreparation &emitter, const std::uint32_t index, const Math::Vec3 origin,
                                 const std::uint32_t color) {
                const Math::Vec3 meshCenter = Right + Math::Vec3{0, 0, AuthoredZ};
                REQUIRE(world->Play(emitter.effect).HasValue());
                const auto particles = world->Particles(emitter.effect, emitter.emitter);
                INFO("particles: " << (particles.HasError() ? particles.ErrorValue().domain.Value() : "") << '/'
                                   << (particles.HasError() ? particles.ErrorValue().code.Value() : "") << ": "
                                   << (particles.HasError() ? particles.ErrorValue().message : "success"));
                REQUIRE(particles.HasValue());
                const auto count = index == 0 || index == 2 ? 2U : 1U;
                for (std::uint32_t particle = 0; particle < count; ++particle)
                    REQUIRE(particles.Value()->Spawn(Horo::Vfx::ParticleSimulationId::Create(index * 8 + particle + 1).Value()).HasValue());
                auto captured = particles.Value()->View();
                INFO("captured: " << (captured.HasError() ? captured.ErrorValue().domain.Value() : "") << '/'
                                  << (captured.HasError() ? captured.ErrorValue().code.Value() : "") << ": "
                                  << (captured.HasError() ? captured.ErrorValue().message : "success"));
                REQUIRE(captured.HasValue());
                for (std::uint32_t particle = 0; particle < count; ++particle) {
                    auto position = origin;
                    if (particle == 1)
                        position = index == 0 ? Math::Vec3{-0.6F, -1.1F, -3} : AlongRay(meshCenter, 1.5F);
                    captured.Value().positionX[particle] = position.x;
                    captured.Value().positionY[particle] = position.y;
                    captured.Value().positionZ[particle] = position.z;
                    const float rayScale = index == 2 ? (particle == 1 ? 1.5F : 0.7F) : index == 3 ? 0.55F : index == 4 ? 0.5F : 1.0F;
                    captured.Value().sizeX[particle] = 0.8F * rayScale;
                    captured.Value().sizeY[particle] = 0.8F * rayScale;
                    captured.Value().rotation[particle] = index == 1 ? 0.35F : 0.17F;
                    captured.Value().packedColor[particle] = particle == 1 && index == 0 ? 0xFFFF00FF : color;
                }
            }

            /** @brief Seeds the fixed seven-particle scene without replacing the real world/extraction authority. */
            void Populate(const std::array<Horo::Vfx::VfxEmitterPreparation, 5> &emitters) {
                const std::array positions{Left, Right, AlongRay(Left, 0.7F), AlongRay(Left, 0.55F), AlongRay(Left, 0.5F)};
                constexpr std::array<std::uint32_t, 5> colors{0xFF0000FF, 0x00FF00FF, 0x0000FF80, 0xFFFFFFFF, 0x00400000};
                for (std::uint32_t index = 0; index < emitters.size(); ++index)
                    PopulateEmitter(emitters[index], index, positions[index], colors[index]);
            }

            explicit Scene(RenderFrontend &frontend) {
                REQUIRE(frontend.PrepareVfxScheduling({64, 1, 1'000'000, 16}).HasValue());
                billboard = Mesh(frontend, 0);
                mesh = Mesh(frontend, AuthoredZ);
                target = Target(frontend);
                for (std::uint32_t index = 0; index < 5; ++index)
                    materials.push_back(Material(frontend, index));
                const auto scoped = Horo::Vfx::VfxIdentityScope::Create(17);
                INFO("scoped: " << (scoped.HasError() ? scoped.ErrorValue().domain.Value() : "") << '/'
                                << (scoped.HasError() ? scoped.ErrorValue().code.Value() : "") << ": "
                                << (scoped.HasError() ? scoped.ErrorValue().message : "success"));
                REQUIRE(scoped.HasValue());
                const auto scope = scoped.Value();
                const auto emitters = Emitters(scope);
                auto preparedWorld = Horo::Vfx::VfxWorld::Prepare(scope, emitters, {16, 64, 128, 2, 1'000'000});
                INFO("preparedWorld: " << (preparedWorld.HasError() ? preparedWorld.ErrorValue().domain.Value() : "") << '/'
                                       << (preparedWorld.HasError() ? preparedWorld.ErrorValue().code.Value() : "") << ": "
                                       << (preparedWorld.HasError() ? preparedWorld.ErrorValue().message : "success"));
                REQUIRE(preparedWorld.HasValue());
                world = std::move(preparedWorld).Value();
                Populate(emitters);
            }
        };

        /** @brief Actual scheduled graph recipe and host-issued draw bindings, retained until execution is complete. */
        struct Recipe {
            RenderGraphBuilder builder;
            VfxGraphSchedule schedule;
            std::vector<VfxGraphDrawBinding> bindings;
            RenderGraphPassRef clearColor, clearDepth;
            RenderGraphResourceId color, depth;
        };

        /** @brief Imports exact mesh/material generations for each actual contiguous scheduled draw run. */
        void BindDraws(Scene &scene, Recipe &recipe) {
            const std::array
                billboardBuffers{recipe.builder.ImportBuffer(scene.billboard.vertices, RenderGraphResourceClass::Persistent).Value(),
                                 recipe.builder.ImportBuffer(scene.billboard.indices, RenderGraphResourceClass::Persistent).Value()};
            const std::array meshBuffers{recipe.builder.ImportBuffer(scene.mesh.vertices, RenderGraphResourceClass::Persistent).Value(),
                                         recipe.builder.ImportBuffer(scene.mesh.indices, RenderGraphResourceClass::Persistent).Value()};
            for (const auto &record : recipe.schedule.Workloads()) {
                const auto &vfx = std::get<RenderGraphVfx>(record.workload);
                const auto batches = vfx.snapshot.Batches();
                for (std::size_t first = 0; first < vfx.order->instances.size();) {
                    const auto &batch = batches[vfx.order->instances[first].batch];
                    auto material = std::ranges::find(scene.materials, batch.material, &MaterialFixture::id);
                    REQUIRE(material != scene.materials.end());
                    const bool authoredMesh = batch.mesh == scene.mesh.mesh;
                    const auto &mesh = authoredMesh ? scene.mesh : scene.billboard;
                    const auto &buffers = authoredMesh ? meshBuffers : billboardBuffers;
                    recipe.bindings.push_back(
                        {.stage = vfx.stage,
                         .firstOrder = static_cast<std::uint32_t>(first),
                         .mesh = mesh.mesh,
                         .target = scene.target.target,
                         .vertices = buffers[0],
                         .indices = buffers[1],
                         .parameters = recipe.builder.ImportBuffer(material->instances, RenderGraphResourceClass::Persistent).Value(),
                         .sampledTexture =
                             recipe.builder.ImportTexture(material->texture.texture, RenderGraphResourceClass::Persistent).Value(),
                         .slot = std::move(material->slot)});
                    const auto batchIndex = vfx.order->instances[first].batch;
                    while (first < vfx.order->instances.size() && vfx.order->instances[first].batch == batchIndex)
                        ++first;
                }
            }
        }

        /** @brief Schedules the real extracted scene and binds each actual contiguous material run without changing its order. */
        Recipe Schedule(RenderFrontend &frontend, Scene &scene, const std::uint64_t frame, const bool bind) {
            auto builder = Test::RequireBuilder({.maxPasses = 16, .maxResources = 32, .maxUsages = 128, .maxDependencies = 64});
            const auto color = builder.ImportTexture(scene.target.color.texture, RenderGraphResourceClass::Persistent).Value();
            const auto depth = builder.ImportTexture(scene.target.depth.texture, RenderGraphResourceClass::Persistent).Value();
            const auto clearColor = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            const auto clearDepth = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            const auto opaqueComplete = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            const auto output = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            Test::RequireUsage(builder, {clearColor, color, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
            Test::RequireUsage(builder, {clearDepth, depth, RenderGraphAccess::Write, RenderGraphUsageKind::DepthStencilAttachment});
            Test::RequireDependency(builder, {clearColor, clearDepth});
            Test::RequireDependency(builder, {clearDepth, opaqueComplete});
            Test::RequireDependency(builder, {opaqueComplete, output});
            const auto forward = Math::Normalize(Math::Cross(scene.view.up, scene.view.right));
            VfxGraphRequest request{.scene = 17,
                                    .frame = frame,
                                    .originRevision = 1,
                                    .view = 2,
                                    .viewGeneration = 3,
                                    .camera = Eye,
                                    .forward = forward,
                                    .maximumInstances = 64,
                                    .remainingDrawCalls = 16,
                                    .opaqueBegin = clearDepth,
                                    .opaqueComplete = opaqueComplete,
                                    .outputTransform = output,
                                    .sceneColor = color,
                                    .sceneDepth = depth};
            const auto extracted = scene.world->Extract(frame, {}, 1);
            const auto scheduled = frontend.ScheduleVfx(builder, extracted.snapshot, request);
            INFO((scheduled.HasError() ? scheduled.ErrorValue().message : "Actual VFX snapshot scheduled"));
            REQUIRE(scheduled.HasValue());
            Recipe recipe{std::move(builder), scheduled.Value(), {}, clearColor, clearDepth, color, depth};
            if (!bind)
                return recipe;
            BindDraws(scene, recipe);
            return recipe;
        }

        /** @brief Compiles the real graph and imports explicit existing resource states; no fixture execution plan is forged. */
        CompiledRenderGraphExecution Compile(Recipe &recipe) {
            const auto graph = Test::RequireGraph(recipe.builder);
            const auto ordered = Test::RequireSchedule(graph);
            std::vector<RenderGraphImportedState> initial;
            for (const auto &resource : graph.Resources()) {
                RenderGraphLogicalState state{.access = RenderGraphSynchronizationAccess::Read,
                                              .operation = RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                              .scope = RenderGraphPipelineScope::Graphics,
                                              .layout = RenderGraphTextureLayout::NotApplicable,
                                              .queue = {1}};
                if (resource.kind == RenderGraphResourceKind::Texture) {
                    state.operation = RenderGraphSynchronizationOperation::Sampled;
                    state.layout = RenderGraphTextureLayout::ShaderReadOnly;
                }
                if (resource.id == recipe.color) {
                    state.access = RenderGraphSynchronizationAccess::ReadWrite;
                    state.operation = RenderGraphSynchronizationOperation::ColorAttachment;
                    state.layout = RenderGraphTextureLayout::ColorAttachment;
                } else if (resource.id == recipe.depth) {
                    state.access = RenderGraphSynchronizationAccess::ReadWrite;
                    state.operation = RenderGraphSynchronizationOperation::DepthStencilAttachment;
                    state.layout = RenderGraphTextureLayout::DepthStencilAttachment;
                }
                initial.push_back({resource.id, state});
            }
            const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
            const auto synchronization = SynthesizeRenderGraphSynchronization(graph, ordered, queues, initial);
            INFO("synchronization: " << (synchronization.HasError() ? synchronization.ErrorValue().domain.Value() : "") << '/'
                                     << (synchronization.HasError() ? synchronization.ErrorValue().code.Value() : "") << ": "
                                     << (synchronization.HasError() ? synchronization.ErrorValue().message : "success"));
            REQUIRE(synchronization.HasValue());
            auto compiled = CompileRenderGraphExecution(graph, ordered, synchronization.Value(), queues);
            INFO("compiled: " << (compiled.HasError() ? compiled.ErrorValue().domain.Value() : "") << '/'
                              << (compiled.HasError() ? compiled.ErrorValue().code.Value() : "") << ": "
                              << (compiled.HasError() ? compiled.ErrorValue().message : "success"));
            REQUIRE(compiled.HasValue());
            return std::move(compiled).Value();
        }

        /** @brief Computes a pixel-center ray/plane depth independently from instance packing or translated shader text. */
        float PlaneDepth(const Math::Mat4 &projection, const int x, const int y, const Math::Vec3 center, const Math::Vec3 normal) {
            const auto inverse = Math::TryInverse(projection);
            INFO("inverse: " << (inverse.HasError() ? inverse.ErrorValue().domain.Value() : "") << '/'
                             << (inverse.HasError() ? inverse.ErrorValue().code.Value() : "") << ": "
                             << (inverse.HasError() ? inverse.ErrorValue().message : "success"));
            REQUIRE(inverse.HasValue());
            const float nx = (static_cast<float>(x) + 0.5F) / 32 - 1;
            const float ny = (static_cast<float>(y) + 0.5F) / 16 - 1;
            const auto near = Math::TransformPoint(inverse.Value(), {nx, ny, -1});
            const auto far = Math::TransformPoint(inverse.Value(), {nx, ny, 1});
            const auto ray = far - near;
            const auto denominator = Math::Dot(normal, ray);
            REQUIRE(std::abs(denominator) > 0.0001F);
            const auto intersection = near + ray * (Math::Dot(normal, center - near) / denominator);
            return (Math::TransformPoint(projection, intersection).z + 1) * 0.5F;
        }

        /** @brief Reads a projected interior reference point and checks independently derived color and plane depth. */
        void Pixel(const Scene &scene, const Math::Vec3 center, const Math::Vec3 normal, const std::array<float, 4> expected,
                   const bool proveAuthoredZ = false) {
            const auto ndc = Math::TransformPoint(scene.view.viewProjection, center);
            const int x = static_cast<int>(std::floor((ndc.x + 1) * 32));
            const int y = static_cast<int>(std::floor((ndc.y + 1) * 16));
            REQUIRE(x > 0);
            REQUIRE(x < 63);
            REQUIRE(y > 0);
            REQUIRE(y < 31);
            std::array<float, 4> actual{};
            float depth{};
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, actual.data());
            glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &depth);
            INFO("Native VFX reference pixel " << x << ',' << y);
            for (std::size_t component = 0; component < actual.size(); ++component)
                CHECK(std::abs(actual[component] - expected[component]) < 0.02F);
            CHECK(std::abs(depth - PlaneDepth(scene.view.viewProjection, x, y, center, normal)) < 0.0001F);
            if (proveAuthoredZ) {
                const auto ignoredZ = PlaneDepth(scene.view.viewProjection, x, y, Right, normal);
                CHECK(std::abs(depth - ignoredZ) > 0.001F);
            }
        }

        /** @brief Requires actual scheduled particle, draw and instanced-draw counts. */
        void VerifyDrawCounts(const Recipe &recipe) {
            std::uint32_t instances{}, draws{}, instancedDraws{};
            for (const auto &record : recipe.schedule.Workloads())
                for (const auto &draw : std::get<RenderGraphVfx>(record.workload).order->draws) {
                    instances += draw.instanceCount;
                    ++draws;
                    if (draw.instanceCount > 1)
                        ++instancedDraws;
                }
            CHECK(instances == 7);
            CHECK(draws == 5);
            CHECK(instancedDraws == 2);
        }

        /** @brief Composes the existing compiled graph's real VFX and attachment workloads. */
        std::vector<RenderGraphPassWorkload> Workloads(const Recipe &recipe, const CompiledRenderGraphExecution &execution) {
            std::vector<RenderGraphPassWorkload> workloads;
            for (const auto &pass : execution.Passes()) {
                auto vfx = std::ranges::find(recipe.schedule.Workloads(), pass.pass, &RenderGraphPassWorkload::pass);
                if (vfx != recipe.schedule.Workloads().end())
                    workloads.push_back(*vfx);
                else if (pass.pass == recipe.clearColor)
                    workloads.push_back({pass.pass, RenderGraphColorAttachment{recipe.color, {}}});
                else if (pass.pass == recipe.clearDepth)
                    workloads.push_back({pass.pass, RenderGraphDepthAttachment{recipe.depth, 1}});
                else
                    workloads.push_back({pass.pass, {}});
            }
            return workloads;
        }

        /** @brief Executes and reads the same real scene while the caller retains graph, material and completion leases. */
        void ExecutePixels(RenderFrontend &frontend, const Scene &scene, const Recipe &recipe,
                           const CompiledRenderGraphExecution &execution, const std::vector<RenderGraphPassWorkload> &workloads) {
            {
                auto begun = frontend.BeginFrame({1, {64, 32}});
                INFO("begun: " << (begun.HasError() ? begun.ErrorValue().domain.Value() : "") << '/'
                               << (begun.HasError() ? begun.ErrorValue().code.Value() : "") << ": "
                               << (begun.HasError() ? begun.ErrorValue().message : "success"));
                REQUIRE(begun.HasValue());
                auto frame = std::move(begun).Value();
                const auto executed = frame.ExecuteGraph(execution, workloads);
                INFO((executed.HasError() ? executed.ErrorValue().message : "Actual VFX graph encoded"));
                REQUIRE(executed.HasValue());
                const auto target = Detail::RenderFrontendResourceAccess::BackendInstance(frontend, scene.target.target);
                INFO("target: " << (target.HasError() ? target.ErrorValue().domain.Value() : "") << '/'
                                << (target.HasError() ? target.ErrorValue().code.Value() : "") << ": "
                                << (target.HasError() ? target.ErrorValue().message : "success"));
                REQUIRE(target.HasValue());
                GLint previous{};
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(target.Value()));
                constexpr float alpha = 128.0F / 255;
                Pixel(scene, Left, {0, 0, 1}, {1 - alpha, 64.0F / 255, alpha, 1});
                Pixel(scene, {-0.6F, -1.1F, -3}, {0, 0, 1}, {1, 1, 0, 1});
                Pixel(scene, Right + Math::Vec3{0, 0, AuthoredZ}, {0, 0, 1}, {0, 128.0F / 255, 0, 1}, true);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous));
                REQUIRE(glGetError() == GL_NO_ERROR);
                REQUIRE(frame.Present().HasValue());
            }
        }

        /** @brief Requires graph CPU leases and real native completion before the same slot generation may be updated. */
        void VerifyPublicationReuse(RenderFrontend &frontend, Scene &scene, Recipe &recipe,
                                    std::vector<RenderGraphPassWorkload> &workloads) {
            const std::array<VfxPackedDrawInstance, 1> replacement{};
            const auto pending = frontend.UpdateVfxFrame(recipe.bindings[0].slot, recipe.bindings[0].slot->Revision() + 1, replacement);
            REQUIRE(pending.HasError());
            CHECK(pending.ErrorValue().code.Value() == VfxFrameErrors::Pending.code.Value());
            // CPU graph leases remain real even if this small GPU workload completed before the query.
            // Explicit test-only completion plus ordinary frame admission establishes actual native retirement.
            glFinish();
            workloads.clear();
            recipe.schedule = {};
            {
                auto begun = frontend.BeginFrame({2, {64, 32}});
                INFO("begun: " << (begun.HasError() ? begun.ErrorValue().domain.Value() : "") << '/'
                               << (begun.HasError() ? begun.ErrorValue().code.Value() : "") << ": "
                               << (begun.HasError() ? begun.ErrorValue().message : "success"));
                REQUIRE(begun.HasValue());
                auto frame = std::move(begun).Value();
                frame.Cancel();
            }
            auto next = Schedule(frontend, scene, 2, false);  // Reuses the sole canonical order slot, releasing its old CPU draw pins.
            CHECK(next.schedule.instances == 7);
            const auto updated = frontend.UpdateVfxFrame(recipe.bindings[0].slot, recipe.bindings[0].slot->Revision() + 1, replacement);
            INFO((updated.HasError() ? updated.ErrorValue().message : "Actual completed native publication reused"));
            REQUIRE(updated.HasValue());
            REQUIRE(glGetError() == GL_NO_ERROR);
        }

        /** @brief Executes actual cooked billboard/mesh shaders and verifies scene-linear blend, depth, masking and retained publication.
         */
        void VerifyVfx(RenderFrontend &frontend) {
            REQUIRE(frontend.Capabilities().support.features.Supports(RenderCapability::VfxForwardInstancing));
            GLint limit{};
            glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &limit);
            std::cout << "Actual VFX GL version=" << reinterpret_cast<const char *>(glGetString(GL_VERSION))
                      << " uniform-block-limit=" << limit << " required=10240\n";
            REQUIRE(limit >= 10240);
            Scene scene{frontend};
            const CompleteOnExit complete;
            auto recipe = Schedule(frontend, scene, 1, true);
            REQUIRE(recipe.bindings.size() == 5);
            const auto prepared = frontend.PrepareVfxDraws(recipe.builder, recipe.schedule, scene.view, recipe.bindings);
            INFO((prepared.HasError() ? prepared.ErrorValue().message : "Exact native VFX draws prepared"));
            REQUIRE(prepared.HasValue());
            VerifyDrawCounts(recipe);
            auto execution = Compile(recipe);
            auto workloads = Workloads(recipe, execution);
            ExecutePixels(frontend, scene, recipe, execution, workloads);
            VerifyPublicationReuse(frontend, scene, recipe, workloads);
        }
    }  // namespace

    TEST_CASE("Cooked native GL VFX billboard and authored-Z mesh preserve off-axis pixels depth and lifetime",
              "[gpu][display][opengl][vfx][hardware-qualification]") {
        WithNativeFrontend(MaterialEvidence::HardwareQualification, VerifyVfx);
    }

    TEST_CASE("Software-capable cooked GL VFX coverage preserves off-axis pixels depth and lifetime",
              "[display][opengl][vfx][software-coverage]") {
        WithNativeFrontend(MaterialEvidence::SoftwareCapableCoverage, VerifyVfx);
    }
}  // namespace Horo::Render::OpenGLMaterialSmoke
