#include "RenderTransientGraphTestSupport.h"
#include "RenderTransientTestBackend.h"

namespace Horo::Render::TransientTest {
    namespace {
        /** @brief Compiles three incompatible native slots so every admission prefix is exercised. */
        [[nodiscard]] GraphSources ThreeQuerySlots() {
            auto builder = Test::RequireBuilder({.maxPasses = 3, .maxResources = 3, .maxUsages = 3, .maxDependencies = 2});
            std::array<RenderGraphTransientRequirement, 3> requirements{};
            std::vector<RenderGraphPassWorkload> workloads;
            RenderGraphPassRef previous;
            for (std::size_t index = 0; index < requirements.size(); ++index) {
                const auto resource = Test::RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Texture));
                const auto pass = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
                Test::RequireUsage(builder, {pass, resource, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
                if (previous.IsValid())
                    Test::RequireDependency(builder, {previous, pass, RenderGraphDependencyKind::ExecutionOrder});
                previous = pass;
                requirements[index] = {resource, RenderTextureDescriptor{.extent = {static_cast<std::uint32_t>(index + 1), 1},
                                                                         .usage = RenderTextureUsage::RenderAttachment}};
                workloads.push_back({pass, RenderGraphColorAttachment{resource, {}}});
            }
            const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
            return CompileSources(builder, requirements, {}, queues, std::move(workloads));
        }

        /** @brief Checks original native failure identity separately from metadata exhaustion translation. */
        template <typename Value> void RequireQueryFailure(const Result<Value> &failed, const QueryFaultKind fault) {
            if (fault == QueryFaultKind::TypedFailure) {
                Test::RequireError(failed, "render.test.transient_native_failure");
                CHECK(failed.ErrorValue().domain.Value() == "render.test");
                CHECK(failed.ErrorValue().severity == ErrorSeverity::Error);
                CHECK(failed.ErrorValue().message == "Injected native resource failure.");
                const auto *cause = failed.ErrorValue().cause.Get();
                REQUIRE(cause != nullptr);
                CHECK(cause->code.Value() == "render.test.native_requirement_cause");
                CHECK(cause->domain.Value() == "render.test.native");
                CHECK(cause->severity == ErrorSeverity::Warning);
                CHECK(cause->message == "Original native requirement evidence.");
            } else
                Test::RequireError(failed, "render.frontend.resource.capacity_exhausted");
        }

        /** @brief Proves a failed ordinary query frees the generation, budget and upload capacity before retry. */
        template <typename Create, typename Release>
        void CheckOrdinaryQuery(const QueryFaultKind fault, const Create &create, const Release &release) {
            auto audit = std::make_shared<Audit>();
            audit->faults.failQuery = 1;
            audit->faults.queryFault = fault;
            auto frontend = MakeFrontend(audit);
            RequireQueryFailure(create(*frontend), fault);
            CHECK(frontend->MemorySnapshot().reservationCount == 0);
            CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
            CHECK(frontend->UploadSnapshot().pendingRequests == 0);
            CHECK(audit->resources.creates == 0);
            const auto retry = create(*frontend);
            REQUIRE(retry.HasValue());
            CHECK(retry.Value().handle.slot == 1);
            REQUIRE(release(*frontend, retry.Value().handle).HasValue());
            CHECK(frontend->MemorySnapshot().reservationCount == 0);
            CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        }
    }  // namespace

    TEST_CASE("Memory-cost callbacks preserve typed failure and bound metadata exceptions during ordinary admission",
              "[renderer][resource][rollback]") {
        constexpr std::array faults{QueryFaultKind::TypedFailure, QueryFaultKind::Allocation, QueryFaultKind::Length};
        for (const auto fault : faults) {
            CheckOrdinaryQuery(fault, [](RenderFrontend &frontend) {
                const std::array<std::byte, 16> bytes{};
                return frontend.CreateBuffer(CopyBuffer(bytes.size()), bytes);
            }, [](RenderFrontend &frontend, const RenderBufferHandle handle) {
                return frontend.ReleaseBuffer(handle);
            });
            CheckOrdinaryQuery(fault, [](RenderFrontend &frontend) {
                return frontend.CreateTexture({.extent = {1, 1}, .usage = RenderTextureUsage::Sampled});
            }, [](RenderFrontend &frontend, const RenderTextureHandle handle) {
                return frontend.ReleaseTexture(handle);
            });
        }
    }

    TEST_CASE("Memory-cost failures roll back first middle and final transient admission prefixes", "[renderer][transient][rollback]") {
        constexpr std::array faults{QueryFaultKind::TypedFailure, QueryFaultKind::Allocation, QueryFaultKind::Length};
        for (const auto fault : faults) {
            for (std::size_t position = 1; position <= 3; ++position) {
                auto audit = std::make_shared<Audit>();
                audit->faults.failQuery = position;
                audit->faults.queryFault = fault;
                auto frontend = MakeFrontend(audit);
                const auto sources = ThreeQuerySlots();
                RequireQueryFailure(frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1}), fault);
                CHECK(audit->resources.queries == position);
                CHECK(audit->resources.creates == 0);
                CHECK(frontend->MemorySnapshot().reservationCount == 0);
                CHECK(frontend->MemorySnapshot().allocationCount == 0);
                CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
                const auto retry = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
                REQUIRE(retry.HasValue());
                CHECK(audit->resources.creates == 3);
                REQUIRE(frontend->ReleaseTransientGraphResources(retry.Value()).HasValue());
                CHECK(audit->resources.destroyed == 3);
                CHECK(frontend->MemorySnapshot().reservationCount == 0);
                CHECK(frontend->MemorySnapshot().allocationCount == 0);
                CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
                frontend.reset();
                CHECK(audit->shutdowns == 1);
                CHECK(audit->resources.destroyed == 3);
            }
        }
    }
}  // namespace Horo::Render::TransientTest
