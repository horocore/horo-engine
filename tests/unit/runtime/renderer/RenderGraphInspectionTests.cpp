#include "Horo/Runtime/Render/RenderGraphInspection.h"
#include "RenderGraphInspectionTestSupport.h"

#include <array>
#include <limits>
#include <nlohmann/json.hpp>
#include <stop_token>
#include <thread>

namespace Horo::Render {
    namespace {
        using namespace Test;

        auto Capture(const Sources &sources, const RenderGraphInspectionContext context = {{41}, {9}, 1},
                     const RenderGraphInspectionLimits limits = {}, const std::stop_token token = {}) {
            return CaptureRenderGraphInspection(sources.graph, sources.schedule, sources.lifetime, sources.execution, context, limits,
                                                token);
        }

        std::shared_ptr<const RenderGraphInspectionSnapshot> DetachedSnapshot() {
            const auto sources = CompileSources();
            auto captured = Capture(sources);
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }

        /** @brief Compiles disjoint compatible lifetimes beside resident identities without pinning native storage. */
        Sources CompileAliasedResidentSources() {
            auto builder = RequireBuilder({.maxPasses = 2, .maxResources = 4, .maxUsages = 2, .maxDependencies = 1});
            const auto first = RequirePass(builder, RenderPassKind::Compute, RenderQueueRole::Compute);
            const auto second = RequirePass(builder, RenderPassKind::Compute, RenderQueueRole::Compute);
            const auto a = RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
            const auto b = RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
            const auto buffer = RequireResource(builder.ImportBuffer(BufferHandle(7), RenderGraphResourceClass::Persistent));
            const auto texture = RequireResource(builder.ImportTexture(TextureHandle(8), RenderGraphResourceClass::History));
            RequireUsage(builder, {first, a, RenderGraphAccess::Write, RenderGraphUsageKind::Storage});
            RequireUsage(builder, {second, b, RenderGraphAccess::Write, RenderGraphUsageKind::Storage});
            RequireDependency(builder, {first, second, RenderGraphDependencyKind::ExecutionOrder});
            auto graph = RequireGraph(builder);
            auto schedule = RequireSchedule(graph);
            const RenderBufferDescriptor descriptor{.byteSize = 256, .usage = RenderBufferUsage::Storage};
            const std::array requirements{RenderGraphTransientRequirement{a, descriptor}, RenderGraphTransientRequirement{b, descriptor}};
            auto lifetime = CompileRenderGraphLifetimePlan(graph, schedule, requirements);
            REQUIRE(lifetime.HasValue());
            const std::array initialStates{RenderGraphImportedState{buffer,
                                                                    {RenderGraphSynchronizationAccess::Read,
                                                                     RenderGraphSynchronizationOperation::Sampled,
                                                                     RenderGraphPipelineScope::External,
                                                                     RenderGraphTextureLayout::NotApplicable,
                                                                     {5}}},
                                           RenderGraphImportedState{texture,
                                                                    {RenderGraphSynchronizationAccess::Read,
                                                                     RenderGraphSynchronizationOperation::Sampled,
                                                                     RenderGraphPipelineScope::External,
                                                                     RenderGraphTextureLayout::ShaderReadOnly,
                                                                     {5}}}};
            return FinishSources(std::move(graph), std::move(schedule), std::move(lifetime).Value(), initialStates);
        }
    }  // namespace

    TEST_CASE("Graph inspection owns complete topology and exact logical queue/lifetime facts after sources retire",
              "[renderer][render-graph][inspection]") {
        const auto snapshot = DetachedSnapshot();
        REQUIRE(snapshot->Passes().size() == 3);
        REQUIRE(snapshot->Execution().size() == 2);
        REQUIRE(snapshot->Resources().size() == 2);
        REQUIRE(snapshot->Usages().size() == 3);
        REQUIRE(snapshot->Exports().size() == 1);
        CHECK(snapshot->Dispositions().back().disposition == RenderGraphPassDispositionKind::Culled);
        CHECK(snapshot->Execution()[0].queue == RenderQueueId{3});
        CHECK(snapshot->Execution()[1].queue == RenderQueueId{5});
        CHECK(snapshot->Lifetimes()[0].firstUseIndex == 0);
        CHECK(snapshot->Lifetimes()[0].lastUseIndex == 1);
        CHECK(snapshot->Lifetimes()[1].disposition == RenderGraphLifetimeDisposition::Unused);
        REQUIRE(snapshot->Allocations().size() == 1);
        REQUIRE(snapshot->Transfers().size() == 1);
        CHECK(snapshot->Transfers()[0].sourceQueue == RenderQueueId{3});
        CHECK(snapshot->Transfers()[0].destinationQueue == RenderQueueId{5});
        REQUIRE(snapshot->Transitions().size() == 2);
        CHECK(HasRenderGraphHazard(snapshot->Transitions()[1].hazards, RenderGraphHazard::ReadAfterWrite));
        CHECK(snapshot->Timing() == RenderGraphInspectionTiming::Unavailable);
    }

    TEST_CASE("Graph debug export is independently parseable deterministic and exact at its byte boundary",
              "[renderer][render-graph][inspection][export]") {
        const auto snapshot = DetachedSnapshot();
        const auto exported = ExportRenderGraphInspection(*snapshot);
        REQUIRE(exported.HasValue());
        const auto parsed = nlohmann::json::parse(exported.Value());
        CHECK(parsed.at("version") == 1);
        CHECK(parsed.at("timing") == "unavailable");
        CHECK(parsed.at("context").at(1) == 9);
        CHECK(parsed.at("passes").size() == 3);
        CHECK(parsed.at("execution").size() == 2);
        CHECK(parsed.at("resources").size() == 2);
        CHECK(parsed.at("uses").size() == 3);
        CHECK(parsed.at("transitions").size() == 2);
        CHECK(parsed.at("transfers").at(0).at(4) == 5);
        CHECK(ExportRenderGraphInspection(*snapshot).Value() == exported.Value());
        CHECK(ExportRenderGraphInspection(*snapshot, exported.Value().size()).Value() == exported.Value());
        RequireError(ExportRenderGraphInspection(*snapshot, exported.Value().size() - 1), "render.graph.inspection.capacity_exceeded");
        RequireError(ExportRenderGraphInspection(*snapshot, 0), "render.graph.inspection.invalid_limits");
        RequireError(ExportRenderGraphInspection(*snapshot, RenderGraphInspectionLimits::HardMaxBytes + 1),
                     "render.graph.inspection.invalid_limits");
    }

    TEST_CASE("Graph inspection rejects mismatched moved and invalid source identities without publishing partial facts",
              "[renderer][render-graph][inspection]") {
        auto first = CompileSources();
        auto second = CompileSources();
        RequireError(CaptureRenderGraphInspection(first.graph, second.schedule, first.lifetime, first.execution, {{41}, {9}, 1}),
                     "render.graph.inspection.invalid_source");
        RequireError(CaptureRenderGraphInspection(first.graph, first.schedule, second.lifetime, first.execution, {{41}, {9}, 1}),
                     "render.graph.inspection.invalid_source");
        RequireError(CaptureRenderGraphInspection(first.graph, first.schedule, first.lifetime, second.execution, {{41}, {9}, 1}),
                     "render.graph.inspection.invalid_source");
        for (const auto context : {RenderGraphInspectionContext{{}, {9}, 1}, RenderGraphInspectionContext{{41}, {}, 1},
                                   RenderGraphInspectionContext{{41}, {9}, 0}})
            RequireError(Capture(first, context), "render.graph.inspection.invalid_source");
        auto moved = std::move(first.graph);
        RequireError(Capture(first), "render.graph.inspection.invalid_source");
        CHECK(moved.Owner().IsValid());
    }

    TEST_CASE("Graph debug export preserves resident handle generations and compatible alias proof without native payloads",
              "[renderer][render-graph][inspection][export]") {
        const auto sources = CompileAliasedResidentSources();
        const auto captured = Capture(sources);
        REQUIRE(captured.HasValue());
        const auto &snapshot = *captured.Value();
        REQUIRE(snapshot.Aliases().size() == 1);
        REQUIRE(snapshot.Allocations().size() == 2);
        CHECK(snapshot.Allocations()[0].slot == snapshot.Allocations()[1].slot);
        const auto exported = ExportRenderGraphInspection(snapshot);
        REQUIRE(exported.HasValue());
        const auto parsed = nlohmann::json::parse(exported.Value());
        CHECK(parsed.at("resources").at(2).at(3) == 1);
        CHECK(parsed.at("resources").at(2).at(4) == 41);
        CHECK(parsed.at("resources").at(2).at(5) == 7);
        CHECK(parsed.at("resources").at(2).at(6) == 1);
        CHECK(parsed.at("resources").at(3).at(3) == 2);
        CHECK(parsed.at("resources").at(3).at(4) == 42);
        CHECK(parsed.at("resources").at(3).at(5) == 8);
        CHECK(parsed.at("resources").at(3).at(6) == 1);
        CHECK(parsed.at("aliases").at(0).at(0) == snapshot.Aliases()[0].first.value);
        CHECK(parsed.at("aliases").at(0).at(1) == snapshot.Aliases()[0].second.value);
        CHECK(parsed.at("aliases").at(0).at(2) == snapshot.Aliases()[0].compatibilityClass.value);
        CHECK(ExportRenderGraphInspection(snapshot, exported.Value().size()).Value() == exported.Value());
    }

    TEST_CASE("Graph capture count bytes and cancelled work preserve the committed publication",
              "[renderer][render-graph][inspection][cancellation]") {
        const auto sources = CompileSources();
        RenderGraphInspectionFeed feed{{41}};
        const auto initial = Capture(sources);
        REQUIRE(initial.HasValue());
        REQUIRE(feed.Publish(initial.Value()).HasValue());
        const auto &snapshot = *initial.Value();
        std::size_t recordCount{};
        std::size_t byteCount{sizeof(RenderGraphInspectionSnapshot)};
        const auto measure = [&](const auto records) {
            recordCount += records.size();
            byteCount += records.size_bytes();
        };
        measure(snapshot.Passes());
        measure(snapshot.Dispositions());
        measure(snapshot.Execution());
        measure(snapshot.Resources());
        measure(snapshot.Exports());
        measure(snapshot.Usages());
        measure(snapshot.Dependencies());
        measure(snapshot.Lifetimes());
        measure(snapshot.Aliases());
        measure(snapshot.Allocations());
        measure(snapshot.Transitions());
        measure(snapshot.Transfers());
        REQUIRE(Capture(sources, {{41}, {9}, 2}, {recordCount, byteCount}).HasValue());
        RequireError(Capture(sources, {{41}, {9}, 2}, {recordCount - 1, byteCount}), "render.graph.inspection.capacity_exceeded");
        RequireError(Capture(sources, {{41}, {9}, 2}, {recordCount, byteCount - 1}), "render.graph.inspection.capacity_exceeded");
        RequireError(Capture(sources, {{41}, {9}, 2}, {.maxRecords = 1}), "render.graph.inspection.capacity_exceeded");
        RequireError(Capture(sources, {{41}, {9}, 2}, {.maxBytes = sizeof(RenderGraphInspectionSnapshot)}),
                     "render.graph.inspection.capacity_exceeded");
        RequireError(Capture(sources, {{41}, {9}, 2}, {.maxRecords = 0}), "render.graph.inspection.invalid_limits");
        RequireError(Capture(sources, {{41}, {9}, 2}, {.maxRecords = RenderGraphInspectionLimits::HardMaxRecords + 1}),
                     "render.graph.inspection.invalid_limits");
        std::stop_source stopped;
        stopped.request_stop();
        RequireError(Capture(sources, {{41}, {9}, 2}, {}, stopped.get_token()), "render.graph.inspection.cancelled");
        RequireError(ExportRenderGraphInspection(*initial.Value(), 4096, stopped.get_token()), "render.graph.inspection.cancelled");
        REQUIRE(feed.Read().HasValue());
        CHECK(feed.Read().Value() == initial.Value());
    }

    TEST_CASE("Inspection renderer replacement and shutdown reject late publication while readers keep owned historical facts",
              "[renderer][render-graph][inspection][lifecycle]") {
        const auto sources = CompileSources();
        RenderGraphInspectionFeed feed{{41}};
        REQUIRE(feed.Read().HasValue());
        CHECK_FALSE(feed.Read().Value());
        const auto first = Capture(sources);
        REQUIRE(first.HasValue());
        REQUIRE(feed.Publish(first.Value()).HasValue());
        const auto retained = feed.Read().Value();
        RequireError(feed.Publish(first.Value()), "render.graph.inspection.stale_publication");
        const auto newer = Capture(sources, {{41}, {10}, 2});
        REQUIRE(newer.HasValue());
        REQUIRE(feed.Publish(newer.Value()).HasValue());
        RequireError(feed.Publish(first.Value()), "render.graph.inspection.stale_publication");
        CHECK(feed.Read().Value() == newer.Value());
        CHECK(retained->Context().frame == FrameToken{9});
        RequireError(feed.Publish({}), "render.graph.inspection.invalid_source");
        RequireError(feed.ReplaceRenderer({41}), "render.graph.inspection.invalid_source");
        RequireError(feed.ReplaceRenderer({}), "render.graph.inspection.invalid_source");
        REQUIRE(feed.ReplaceRenderer({42}).HasValue());
        CHECK_FALSE(feed.Read().Value());
        RequireError(feed.Publish(first.Value()), "render.graph.inspection.stale_publication");
        const auto replacement = Capture(sources, {{42}, {1}, 1});
        REQUIRE(replacement.HasValue());
        REQUIRE(feed.Publish(replacement.Value()).HasValue());
        REQUIRE(feed.Shutdown().HasValue());
        REQUIRE(feed.Shutdown().HasValue());
        RequireError(feed.Read(), "render.graph.inspection.closed");
        RequireError(feed.Publish(replacement.Value()), "render.graph.inspection.closed");
        RequireError(feed.ReplaceRenderer({43}), "render.graph.inspection.closed");
        CHECK(retained->Context().renderer == RenderResourceOwnerId{41});
        CHECK(ExportRenderGraphInspection(*retained).HasValue());
    }

    TEST_CASE("Inspection compilation may run on a worker but publication reads replacement and shutdown remain owner-thread operations",
              "[renderer][render-graph][inspection][affinity]") {
        const auto sources = CompileSources();
        RenderGraphInspectionFeed feed{{41}};
        std::array<bool, 5> checks{};
        std::jthread worker{[&] {
            const auto captured = Capture(sources);
            checks[0] = captured.HasValue();
            if (!checks[0])
                return;
            const auto wrongThread = [](const auto &result) {
                return result.HasError() && result.ErrorValue().code.Value() == "render.graph.inspection.wrong_thread";
            };
            checks[1] = wrongThread(feed.Publish(captured.Value()));
            checks[2] = wrongThread(feed.Read());
            checks[3] = wrongThread(feed.ReplaceRenderer({42}));
            checks[4] = wrongThread(feed.Shutdown());
        }};
        worker.join();
        for (const bool check : checks)
            CHECK(check);
        CHECK_FALSE(feed.Read().Value());
        REQUIRE(feed.Publish(DetachedSnapshot()).HasValue());
    }
}  // namespace Horo::Render
