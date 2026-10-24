#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "RenderExternalImages.h"
#include "RenderFrontendTestSupport.h"
#include "RenderGraphTemplate.h"
#include "support/AllocationProbe.h"

namespace Horo::Tests::RenderFrontendTesting {
    namespace {
        using namespace Render::Detail;
        constexpr std::array Queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};

        class ExternalSource final : public IRenderExternalTextureSource {
        public:
            explicit ExternalSource(const std::uint64_t device) : device_(device) {}

            Result<void> Validate() const override {
                return live ? Result<void>::Success() : Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
            }

            std::uint64_t DeviceIncarnation() const noexcept override {
                return device_;
            }

            const RenderTextureDescriptor &Descriptor() const noexcept override {
                return descriptor;
            }

            RenderGraphLogicalState AcquiredState() const noexcept override {
                return acquired;
            }

            RenderGraphLogicalState RequiredFinalState() const noexcept override {
                auto result = acquired;
                result.scope = RenderGraphPipelineScope::External;
                return result;
            }

            RenderTextureDescriptor descriptor{.extent = {64, 64},
                                               .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled};
            RenderGraphLogicalState acquired{RenderGraphSynchronizationAccess::ReadWrite,
                                             RenderGraphSynchronizationOperation::ColorAttachment,
                                             RenderGraphPipelineScope::Graphics,
                                             RenderGraphTextureLayout::ColorAttachment,
                                             {1}};
            bool live{true};

        private:
            std::uint64_t device_;
        };

        struct TemplateFixture {
            std::unique_ptr<RenderFrontend> frontend;
            std::shared_ptr<ExternalSource> source;
            RenderTextureHandle texture;
            RenderGraphExternalTextureSlot slot;
            std::optional<PreparedRenderGraphTemplate> topology;
            std::array<RenderBufferHandle, 2> readBuffers{};
            std::array<RenderGraphOptionalRead, 2> reads{};

            explicit TemplateFixture(bool optionalReads = false) {
                lifecycleState = {};
                frontend = CreateTrackingFrontend();
                source = std::make_shared<ExternalSource>(frontend->Capabilities().support.deviceIncarnation);
                auto imported = RenderExternalTextureAccess::Import(*frontend, source);
                REQUIRE(imported.HasValue());
                texture = imported.Value();
                auto builder = Render::Test::RequireBuilder();
                const auto pass = Render::Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
                auto declared = PreparedRenderGraphTemplate::DeclareTexture(builder, source->Descriptor(), source->AcquiredState(),
                                                                            source->RequiredFinalState(), source->DeviceIncarnation());
                REQUIRE(declared.HasValue());
                slot = declared.Value();
                Render::Test::RequireUsage(builder, {pass, slot.resource, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
                std::array<RenderGraphImportedState, 2> inputs{};
                if (optionalReads) {
                    const std::array<std::byte, 16> bytes{};
                    for (std::size_t index = 0; index < reads.size(); ++index) {
                        const auto usage = index == 0 ? RenderBufferUsage::Vertex : RenderBufferUsage::Index;
                        const auto created = frontend->CreateBuffer({bytes.size(), usage, RenderBufferAccess::HostVisible}, bytes);
                        REQUIRE(created.HasValue());
                        readBuffers[index] = created.Value().handle;
                        const auto resource = builder.ImportBuffer(readBuffers[index], RenderGraphResourceClass::Persistent);
                        REQUIRE(resource.HasValue());
                        const auto role = index == 0 ? RenderGraphUsageKind::VertexBuffer : RenderGraphUsageKind::IndexBuffer;
                        reads[index] = {resource.Value(), role};
                        Render::Test::RequireUsage(builder, {pass, resource.Value(), RenderGraphAccess::Read, role});
                        inputs[index] = {resource.Value(),
                                         {RenderGraphSynchronizationAccess::Read,
                                          RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                          RenderGraphPipelineScope::Graphics,
                                          RenderGraphTextureLayout::NotApplicable,
                                          {1}}};
                    }
                    REQUIRE(frontend->ProcessResourceRequests().HasValue());
                }
                auto prepared = PreparedRenderGraphTemplate::Prepare(Render::Test::RequireGraph(builder), std::array{slot},
                                                                     optionalReads ? std::span<const RenderGraphImportedState>{inputs}
                                                                                   : std::span<const RenderGraphImportedState>{},
                                                                     Queues, 2,
                                                                     optionalReads ? std::span<const RenderGraphOptionalRead>{reads}
                                                                                   : std::span<const RenderGraphOptionalRead>{});
                REQUIRE(prepared.HasValue());
                topology.emplace(std::move(prepared).Value());
            }

            Result<const CompiledRenderGraphExecution *> Bind() {
                return topology->Bind(*frontend, 0, std::array{RenderGraphExternalTextureBinding{slot.resource, texture}});
            }
        };

        class EmptyGui final : public IGuiPassExecutor {
        public:
            Result<GuiCompositionPreparation> PrepareComposition(const FrameToken frame) override {
                return Result<GuiCompositionPreparation>::Success({frame, GuiCompositionMode::BorrowedActiveRecording, {}, {}});
            }

            Result<void> RenderDrawData(const GuiCompositionRequest &) override {
                return Result<void>::Success();
            }
        };
    }  // namespace

    TEST_CASE_METHOD(TemplateFixture, "Bound external topology preserves ordinary graph uses and external releases",
                     "[renderer][render-graph][template]") {
        const auto bound = Bind();
        REQUIRE(bound.HasValue());
        auto builder = Render::Test::RequireBuilder();
        const auto pass = Render::Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const auto resource = Render::Test::RequireResource(builder.ImportTexture(texture, RenderGraphResourceClass::External));
        Render::Test::RequireUsage(builder, {pass, resource, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
        const auto graph = Render::Test::RequireGraph(builder);
        const auto schedule = Render::Test::RequireSchedule(graph);
        const auto sync = SynthesizeRenderGraphSynchronization(graph, schedule, Queues,
                                                               std::array{RenderGraphImportedState{resource, source->AcquiredState()}},
                                                               std::array{RenderGraphFinalState{resource, source->RequiredFinalState()}});
        REQUIRE(sync.HasValue());
        auto ordinary = CompileRenderGraphExecution(graph, schedule, sync.Value(), Queues);
        REQUIRE(ordinary.HasValue());
        CHECK(bound.Value()->Resources().front().binding == ordinary.Value().Resources().front().binding);
        CHECK(bound.Value()->Passes().size() == ordinary.Value().Passes().size());
        CHECK(bound.Value()->Usages().front().kind == ordinary.Value().Usages().front().kind);
        CHECK(bound.Value()->Transitions().size() == ordinary.Value().Transitions().size());
        REQUIRE(bound.Value()->ExternalReleases().size() == 1);
        CHECK(bound.Value()->ExternalReleases().front().newState == ordinary.Value().ExternalReleases().front().newState);
    }

    TEST_CASE_METHOD(TemplateFixture, "External template partial stale and incompatible binds cannot remain submitable",
                     "[renderer][render-graph][template]") {
        auto bound = Bind();
        REQUIRE(bound.HasValue());
        SECTION("missing acquisition") {
            CHECK(topology->Bind(*frontend, 0, {}).HasError());
        }
        SECTION("stale generation") {
            ++texture.generation;
            CHECK(Bind().HasError());
        }
        SECTION("native acquisition revoked") {
            source->live = false;
            CHECK(Bind().HasError());
        }
        SECTION("same image changed layout") {
            source->acquired.layout = RenderGraphTextureLayout::ShaderReadOnly;
            CHECK(Bind().HasError());
        }
        SECTION("same image changed descriptor") {
            source->descriptor.extent.width = 128;
            CHECK(Bind().HasError());
        }
        CHECK_FALSE(bound.Value()->Owner().IsValid());
    }

    TEST_CASE_METHOD(TemplateFixture, "External template storage remains immutable until the same scene and GUI lease completes",
                     "[renderer][render-graph][template][gui]") {
        lifecycleState.completeGraphOnPresent = false;
        const auto bound = Bind();
        REQUIRE(bound.HasValue());
        auto begun = frontend->BeginFrame({1, {64, 64}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        REQUIRE(frame.ExecuteGraph(*bound.Value(), {}).HasValue());
        auto *sceneLease = lifecycleState.graphLease;
        REQUIRE(sceneLease != nullptr);
        EmptyGui gui;
        REQUIRE(frame.ComposeGui(gui).HasValue());
        CHECK(lifecycleState.graphLease == sceneLease);
        REQUIRE(frame.Present().HasValue());
        CHECK(Bind().HasError());
        CHECK(bound.Value()->Owner().IsValid());
        sceneLease->Release();
        lifecycleState.graphLease = nullptr;
        REQUIRE(Bind().HasValue());
    }

    TEST_CASE_METHOD(TemplateFixture, "Aborted graph template admission releases its canonical storage without a new completion clock",
                     "[renderer][render-graph][template]") {
        const auto bound = Bind();
        REQUIRE(bound.HasValue());
        {
            auto begun = frontend->BeginFrame({1, {64, 64}});
            REQUIRE(begun.HasValue());
            auto frame = std::move(begun).Value();
            REQUIRE(frame.ExecuteGraph(*bound.Value(), {}).HasValue());
        }
        CHECK(lifecycleState.abortCount == 1);
        REQUIRE(Bind().HasValue());
    }

    TEST_CASE_METHOD(TemplateFixture, "Warmed successful external graph binding performs no owner-thread C++ allocation",
                     "[renderer][render-graph][template][allocation]") {
        REQUIRE(Bind().HasValue());
        bool success = true;
        AllocationProbe::Measurement observed;
        {
            const AllocationProbe::ScopedMeasurement measure;
            for (std::size_t frame = 0; frame < 64; ++frame)
                success = success && Bind().HasValue();
            observed = measure.Snapshot();
        }
        CHECK(success);
        CHECK(observed.requests == 0);
        CHECK(observed.requestedBytes == 0);
        // Scope is registry validation + preallocated binding only; native import/views and draw encoding are separate gates.
    }
}  // namespace Horo::Tests::RenderFrontendTesting

namespace Horo::Tests::RenderFrontendTesting {
    TEST_CASE("External graph declarations and cold instance storage reject capacities without fallback",
              "[renderer][render-graph][template]") {
        using namespace Render::Detail;
        auto builder = Render::Test::RequireBuilder({1, 1, 1, 1});
        const auto pass = Render::Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const ExternalSource source{1};
        CHECK(PreparedRenderGraphTemplate::DeclareTexture(builder, source.Descriptor(), source.AcquiredState(), source.RequiredFinalState(),
                                                          0)
                  .HasError());
        auto declared = PreparedRenderGraphTemplate::DeclareTexture(builder, source.Descriptor(), source.AcquiredState(),
                                                                    source.RequiredFinalState(), 1);
        REQUIRE(declared.HasValue());
        CHECK(declared.Value().resource.value == 1);
        CHECK(PreparedRenderGraphTemplate::DeclareTexture(builder, source.Descriptor(), source.AcquiredState(), source.RequiredFinalState(),
                                                          1)
                  .HasError());
        Render::Test::RequireUsage(builder,
                                   {pass, declared.Value().resource, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
        const auto excessive =
            PreparedRenderGraphTemplate::Prepare(Render::Test::RequireGraph(builder), std::array{declared.Value()}, {}, Queues, 9);
        CHECK(excessive.HasError());
    }

    TEST_CASE("Template optional reads bind empty subset and full exact uses without allocating", "[renderer][render-graph][template]") {
        TemplateFixture f{true};
        const std::array textures{RenderGraphExternalTextureBinding{f.slot.resource, f.texture}};
        for (std::size_t count = 0; count <= f.reads.size(); ++count) {
            const auto allocation = AllocationProbe::Count();
            const auto bound = f.topology->Bind(*f.frontend, 0, textures, std::span{f.reads}.first(count));
            const auto after = AllocationProbe::Count();
            REQUIRE(bound.HasValue());
            REQUIRE(after == allocation);
            REQUIRE(bound.Value()->Usages().size() == count + 1);
            REQUIRE(bound.Value()->Passes().front().usages.count == count + 1);
            REQUIRE(bound.Value()->Usages().front().kind == RenderGraphUsageKind::ColorAttachment);
            for (std::size_t index = 0; index < count; ++index) {
                REQUIRE(bound.Value()->Usages()[index + 1].resource == f.reads[index].resource);
                REQUIRE(bound.Value()->Usages()[index + 1].kind == f.reads[index].role);
            }
        }
        REQUIRE(f.topology->Bind(*f.frontend, 0, textures).Value()->Usages().size() == 3);
    }

    TEST_CASE("Template optional read rejection preserves queued storage and rejects stale unselected barriers",
              "[renderer][render-graph][template]") {
        TemplateFixture f{true};
        const std::array textures{RenderGraphExternalTextureBinding{f.slot.resource, f.texture}};
        auto selected = f.reads;
        SECTION("duplicate") {
            selected[1] = selected[0];
        }
        SECTION("undeclared attachment") {
            selected[0] = {f.slot.resource, RenderGraphUsageKind::VertexBuffer};
        }
        SECTION("foreign owner") {
            ++selected[0].resource.owner.value;
        }
        SECTION("wrong read role") {
            selected[0].role = RenderGraphUsageKind::UniformBuffer;
        }
        SECTION("write role") {
            selected[0].role = RenderGraphUsageKind::ColorAttachment;
        }
        SECTION("stale unused read still belongs to the synchronization superset") {
            REQUIRE(f.frontend->ReleaseBuffer(f.readBuffers[1]).HasValue());
            REQUIRE(f.frontend->ProcessResourceRequests().HasValue());
            REQUIRE(f.topology->Bind(*f.frontend, 0, textures, std::span{selected}.first(1)).HasError());
            return;
        }
        REQUIRE(f.topology->Bind(*f.frontend, 0, textures, selected).HasError());
    }

    TEST_CASE("Template queued optional selection remains immutable through scene and late GUI completion",
              "[renderer][render-graph][template][gui]") {
        TemplateFixture f{true};
        lifecycleState.completeGraphOnPresent = false;
        const std::array textures{RenderGraphExternalTextureBinding{f.slot.resource, f.texture}};
        const auto bound = f.topology->Bind(*f.frontend, 0, textures, std::span{f.reads}.first(1));
        REQUIRE(bound.HasValue());
        auto opened = f.frontend->BeginFrame({1, {64, 64}});
        REQUIRE(opened.HasValue());
        auto frame = std::move(opened).Value();
        REQUIRE(frame.ExecuteGraph(*bound.Value(), {}).HasValue());
        EmptyGui gui;
        REQUIRE(frame.ComposeGui(gui).HasValue());
        REQUIRE(frame.Present().HasValue());
        REQUIRE(f.topology->Bind(*f.frontend, 0, textures, {}).HasError());
        REQUIRE(bound.Value()->Owner().IsValid());
        REQUIRE(bound.Value()->Usages().size() == 2);
        REQUIRE(lifecycleState.graphLease != nullptr);
        lifecycleState.graphLease->Release();
        lifecycleState.graphLease = nullptr;
        REQUIRE(f.frontend->ProcessResourceRequests().HasValue());
        REQUIRE(f.topology->Bind(*f.frontend, 0, textures, {}).HasValue());
    }
}  // namespace Horo::Tests::RenderFrontendTesting
