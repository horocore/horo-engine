#pragma once
#include "Horo/Runtime/Render/MultiviewShaderParameters.h"
#include "RenderGraphSubmissionFixture.h"

namespace Horo::Render::MultiviewTest {
    /** @brief Deterministic adapter borrows a real prepared native packet for canonical table-issued generations. */
    struct BorrowedMaterialBackend final : IMaterialBindingBackend {
        struct Lease final : IResidentMaterialBinding {
            const IResidentMaterialBinding *native;

            explicit Lease(const IResidentMaterialBinding &value) : native(&value) {}

            const IResidentMaterialBinding &NativeBinding() const noexcept override {
                return *native;
            }
        };

        const IResidentMaterialBinding *native{};

        Result<std::unique_ptr<IResidentMaterialBinding>> Realize(const MaterialBindingDescriptor &) override {
            return Result<std::unique_ptr<IResidentMaterialBinding>>::Success(std::make_unique<Lease>(*native));
        }
    };

    /** @brief Two-view final-artifact layout with padded matrices and scalar routing arrays. */
    inline MaterialBindingLayout Layout(const bool columnMajor = true) {
        MaterialBindingLayout layout;
        layout.backend = ShaderTargetBackend::Vulkan;
        layout.resources = {
            {{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Vertex, true}};
        layout.targetBindings = {{{1}, 0, 0, 0, "", {}, true}};
        layout.parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 2, 0, 64, 16, columnMajor, true},
                             {{2}, {1}, ShaderValueType::Float32, 4, 4, 1, 128, 0, 16, columnMajor, true},
                             {{3}, {1}, ShaderValueType::Uint32, 1, 1, 2, 192, 16, 0, false, true}};
        return layout;
    }

    struct GraphPacket {
        CompiledRenderGraphExecution graph;
        RenderGraphResourceId color, vertex, index, uniform;
    };

    /** @brief Uses real portable authoring/synchronization for both injected and native GPU qualification. */
    inline GraphPacket Graph(const RenderTextureHandle colorHandle, const RenderBufferHandle vertexHandle,
                             const RenderBufferHandle indexHandle, const RenderBufferHandle uniformHandle) {
        auto builder = SubmissionTest::Take(RenderGraphBuilder::Create());
        const auto pass = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto color = SubmissionTest::Take(builder.ImportTexture(colorHandle, RenderGraphResourceClass::External));
        const auto vertex = SubmissionTest::Take(builder.ImportBuffer(vertexHandle, RenderGraphResourceClass::Persistent));
        const auto index = SubmissionTest::Take(builder.ImportBuffer(indexHandle, RenderGraphResourceClass::Persistent));
        const auto uniform = SubmissionTest::Take(builder.ImportBuffer(uniformHandle, RenderGraphResourceClass::Persistent));
        REQUIRE(builder.ExportResource(color).HasValue());
        REQUIRE(builder.AddUsage({pass, color, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment}).HasValue());
        REQUIRE(builder.AddUsage({pass, vertex, RenderGraphAccess::Read, RenderGraphUsageKind::VertexBuffer}).HasValue());
        REQUIRE(builder.AddUsage({pass, index, RenderGraphAccess::Read, RenderGraphUsageKind::IndexBuffer}).HasValue());
        REQUIRE(builder.AddUsage({pass, uniform, RenderGraphAccess::Read, RenderGraphUsageKind::UniformBuffer}).HasValue());
        auto authored = SubmissionTest::Take(builder.Finalize());
        const auto schedule = SubmissionTest::Take(CompileRenderGraph(authored));
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        const RenderGraphLogicalState input{RenderGraphSynchronizationAccess::Read,
                                            RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                            RenderGraphPipelineScope::Graphics,
                                            RenderGraphTextureLayout::NotApplicable,
                                            {1}};
        const std::array initial{RenderGraphImportedState{color, {}, RenderGraphInitialContents::Discarded},
                                 RenderGraphImportedState{vertex, input}, RenderGraphImportedState{index, input},
                                 RenderGraphImportedState{uniform, input}};
        const auto sync = SubmissionTest::Take(SynthesizeRenderGraphSynchronization(authored, schedule, queues, initial));
        return {SubmissionTest::Take(CompileRenderGraphExecution(authored, schedule, sync, queues)), color, vertex, index, uniform};
    }
}  // namespace Horo::Render::MultiviewTest
