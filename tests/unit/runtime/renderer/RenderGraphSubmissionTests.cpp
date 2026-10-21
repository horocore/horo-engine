#include "../../../support/renderer/RenderGraphSubmissionFixture.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

namespace Horo::Render::SubmissionTest {
    TEST_CASE("Canonical graph admission accepts exact ordered ownership signal-wait partitions", "[renderer][render-graph][submission]") {
        auto packet = CopyGraph();
        auto records = packet.Submissions();
        REQUIRE(packet.graph.ReleaseTransfers().size() == 1);
        REQUIRE(packet.graph.AcquireTransfers().size() == 1);
        const RenderGraphExecutionRequest request{{1}, packet.graph, packet.workloads, packet.instances, nullptr, false, nullptr, records};
        CHECK(ValidateRenderGraphExecutionRequest(request).HasValue());
        auto moved = std::move(packet.graph);
        CHECK_FALSE(packet.graph.Owner().IsValid());
        REQUIRE(moved.ImportedStates().size() == 3);
        CHECK(moved.ImportedStates()[1].contents == RenderGraphInitialContents::Discarded);
    }

    TEST_CASE("Canonical graph admission rejects incomplete partitions and missing exact native ownership waits",
              "[renderer][render-graph][submission]") {
        auto packet = CopyGraph();
        auto records = packet.Submissions();
        bool legacy = false;
        SECTION("legacy single queue route cannot ignore transfers") {
            legacy = true;
        }
        SECTION("missing producer signal wait") {
            records[1].submission.waits = {};
        }
        SECTION("wrong producer signal point") {
            packet.producerWait[0].value = 2;
        }
        SECTION("reversed global admission order") {
            records[1].submission.order = {1};
        }
        SECTION("wrong native queue mapping") {
            records[1].submission.queue = {1};
            records[1].submission.signal.queue = {1};
        }
        SECTION("nonexhaustive partition") {
            records[1].passes.count = 0;
        }
        SECTION("partition gap") {
            records[1].passes.offset = 2;
        }
        SECTION("self/future wait") {
            packet.producerWait[0] = records[1].submission.signal;
        }
        const RenderGraphExecutionRequest request{{1},
                                                  packet.graph,
                                                  packet.workloads,
                                                  packet.instances,
                                                  nullptr,
                                                  false,
                                                  nullptr,
                                                  legacy ? std::span<const RenderGraphQueueSubmission>{}
                                                         : std::span<const RenderGraphQueueSubmission>{records}};
        CHECK(ValidateRenderGraphExecutionRequest(request).HasError());
    }

    TEST_CASE("Explicit submission authority cannot regress a single effective queue signal", "[renderer][render-graph][submission]") {
        auto packet = CopyGraph({{{{41}, 1, 1}, {{41}, 2, 1}, {{41}, 3, 1}}}, {1}, {1});
        auto records = packet.Submissions();
        REQUIRE(
            ValidateRenderGraphExecutionRequest({{1}, packet.graph, packet.workloads, packet.instances, nullptr, false, nullptr, records})
                .HasValue());
        records[1].submission.signal.value = 1;
        CHECK(ValidateRenderGraphExecutionRequest({{1}, packet.graph, packet.workloads, packet.instances, nullptr, false, nullptr, records})
                  .HasError());
    }
}  // namespace Horo::Render::SubmissionTest
