#include "RenderTransientGraphTestSupport.h"
#include "RenderTransientTestBackend.h"
#include "UiSubmissionSourceFixture.h"

namespace Horo::Render::TransientTest {
    TEST_CASE("Transient and UI generations share the exact native completion lease", "[renderer][transient][runtime_ui]") {
        auto audit = std::make_shared<Audit>();
        audit->faults.delayCompletion = true;
        auto frontend = MakeFrontend(audit);
        Test::UiSubmissionSources ui;
        const std::array<std::byte, 64> imageBytes{};
        const std::array<std::byte, 128> atlasBytes{};
        auto image = std::optional{Test::UiRequire(frontend->CreateUiImageTexture(ui.images, ui.image, 0, imageBytes))};
        auto atlas = std::optional{Test::UiRequire(frontend->CreateUiGlyphAtlasTexture(ui.atlas, ui.atlas.Pages().front(), atlasBytes))};
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        auto sources = ColorGraph({image->Creation().handle, atlas->Creation().handle});
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        const std::array images{UiRenderImageBinding{&ui.images, ui.image, 0, sources.execution.Resources()[0].id, &*image}};
        const std::array fonts{UiRenderFontBinding{&*ui.font, sources.execution.Resources()[1].id, &*atlas}};
        UiRenderSubmission request{&*ui.geometry, images, fonts, Test::UiRequire(ui.atlas.SealFrame(ui.frame))};
        auto frame = BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value(), std::move(request)).HasValue());
        REQUIRE(frame.Present().HasValue());
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        REQUIRE(frontend->ReleaseTexture(image->Creation().handle).HasValue());
        REQUIRE(frontend->ReleaseTexture(atlas->Creation().handle).HasValue());
        image.reset();
        atlas.reset();
        ui.CloseSources();
        REQUIRE_FALSE(ui.images.IsDrained());
        REQUIRE_FALSE(ui.arena.IsDrained());
        REQUIRE_FALSE(ui.atlas.IsDrained());
        CHECK(audit->resources.destroyed == 0);
        audit->Complete();
        REQUIRE(ui.images.IsDrained());
        REQUIRE(ui.arena.IsDrained());
        REQUIRE(ui.atlas.IsDrained());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(audit->resources.destroyed == 3);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
    }

    TEST_CASE("Combined imported and transient execution admits only the actual three backing pins", "[renderer][transient][execution]") {
        auto audit = std::make_shared<Audit>();
        RenderFrontendMemoryConfig memory;
        memory.budget.hardCapBytes = 48;
        memory.budget.defaultBlockBytes = 16;
        memory.budget.maximumBlockBytes = 48;
        auto frontend = MakeFrontend(audit, memory, {.maximumSubmissionPins = 3});
        const auto buffers = ImportedCopyBuffers(*frontend);
        auto sources = CopyGraph(buffers[0], buffers[1], CopyShape::Aliased, true);
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 48);
        auto frame = BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        REQUIRE(audit->instanceCount == 5);
        CHECK(audit->instances[2].instance == audit->instances[3].instance);
        CHECK(audit->instances[4].instance == 0);
        REQUIRE(frame.Present().HasValue());
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 32);
        REQUIRE(frontend->ReleaseBuffer(buffers[0]).HasValue());
        REQUIRE(frontend->ReleaseBuffer(buffers[1]).HasValue());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CHECK(audit->resources.destroyed == 3);
    }

    TEST_CASE("Transient missing admission and mismatched workloads never encode native work", "[renderer][transient][validation]") {
        auto audit = std::make_shared<Audit>();
        auto frontend = MakeFrontend(audit);
        auto sources = ColorGraph();
        auto frame = BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads).HasError());
        CHECK(audit->instanceCount == 0);
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        auto invalid = BeginFrame(*frontend, 2);
        sources.workloads[0].workload = std::monostate{};
        REQUIRE(invalid.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasError());
        CHECK(audit->retained == nullptr);
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
    }

    TEST_CASE("Transient set record capacity is bounded and released records receive fresh identities",
              "[renderer][transient][validation]") {
        auto audit = std::make_shared<Audit>();
        auto frontend = MakeFrontend(audit);
        auto sources = ColorGraph();
        std::array<RenderGraphTransientResourcesHandle, 8> sets;
        for (auto &set : sets) {
            const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
            REQUIRE(prepared.HasValue());
            set = prepared.Value();
        }
        REQUIRE(frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1}).HasError());
        CHECK(audit->resources.creates == sets.size());
        REQUIRE(frontend->ReleaseTransientGraphResources(sets[0]).HasValue());
        const auto replacement = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value() != sets[0]);
        CHECK(frontend->ReleaseTransientGraphResources(sets[0]).HasError());
        REQUIRE(frontend->ReleaseTransientGraphResources(replacement.Value()).HasValue());
        for (std::size_t index = 1; index < sets.size(); ++index)
            REQUIRE(frontend->ReleaseTransientGraphResources(sets[index]).HasValue());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
    }

    TEST_CASE("Unsubmitted transient frame abandonment returns the same set to readiness", "[renderer][transient][completion]") {
        auto audit = std::make_shared<Audit>();
        auto frontend = MakeFrontend(audit);
        auto sources = ColorGraph();
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        {
            auto frame = BeginFrame(*frontend);
            REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        }
        CHECK(audit->retained == nullptr);
        auto replacement = BeginFrame(*frontend, 2);
        REQUIRE(replacement.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        REQUIRE(replacement.Present().HasValue());
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
    }

    TEST_CASE("Transient alias slots are realized once and charged once", "[renderer][transient][allocation]") {
        auto audit = std::make_shared<Audit>();
        auto frontend = MakeFrontend(audit);
        auto sources = CopyGraph();
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        CHECK(audit->resources.creates == 1);
        CHECK(audit->resources.queriesAtFirstCreate == 1);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 16);
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        CHECK(audit->resources.destroyed == 1);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CHECK(frontend->ReleaseTransientGraphResources(prepared.Value()).HasError());
    }

    TEST_CASE("Transient incompatible overlapping and cross-role resources retain distinct storage", "[renderer][transient][allocation]") {
        for (const auto shape : {CopyShape::Incompatible, CopyShape::Overlapping, CopyShape::DifferentRoles}) {
            auto audit = std::make_shared<Audit>();
            auto frontend = MakeFrontend(audit);
            auto sources = CopyGraph({{41}, 1, 1}, {{41}, 2, 1}, shape);
            const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
            REQUIRE(prepared.HasValue());
            CHECK(audit->resources.creates == 2);
            CHECK(audit->resources.queriesAtFirstCreate == 2);
            CHECK(sources.lifetime.AliasOpportunities().empty());
            CHECK(frontend->MemorySnapshot().committedBackingBytes == (shape == CopyShape::Incompatible ? 48 : 32));
            REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
            CHECK(audit->resources.destroyed == 2);
            CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        }
    }

    TEST_CASE("Transient full-set budget denial precedes every native allocation", "[renderer][transient][rollback]") {
        auto audit = std::make_shared<Audit>();
        RenderFrontendMemoryConfig memory;
        memory.budget.hardCapBytes = 32;
        memory.budget.defaultBlockBytes = 16;
        memory.budget.maximumBlockBytes = 32;
        auto frontend = MakeFrontend(audit, memory);
        auto sources = CopyGraph({{41}, 1, 1}, {{41}, 2, 1}, CopyShape::Incompatible);
        REQUIRE(frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1}).HasError());
        CHECK(audit->resources.queries == 2);
        CHECK(audit->resources.creates == 0);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
    }

    TEST_CASE("Transient partial native failure retires admitted siblings and preserves the cause", "[renderer][transient][rollback]") {
        auto audit = std::make_shared<Audit>();
        audit->faults.failCreate = 2;
        auto frontend = MakeFrontend(audit);
        auto sources = CopyGraph({{41}, 1, 1}, {{41}, 2, 1}, CopyShape::Incompatible);
        const auto failed = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        Test::RequireError(failed, "render.test.transient_native_failure");
        CHECK(audit->resources.queriesAtFirstCreate == 2);
        CHECK(audit->resources.creates == 2);
        CHECK(audit->resources.destroyed == 1);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
        audit->faults.failCreate = 0;
        REQUIRE(frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1}).HasValue());
    }

    TEST_CASE("Transient typed native requirement failure cancels the complete reserved prefix", "[renderer][transient][rollback]") {
        auto audit = std::make_shared<Audit>();
        audit->faults.failQuery = 2;
        auto frontend = MakeFrontend(audit);
        auto sources = CopyGraph({{41}, 1, 1}, {{41}, 2, 1}, CopyShape::Incompatible);
        const auto failed = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        Test::RequireError(failed, "render.test.transient_native_failure");
        CHECK(failed.ErrorValue().domain.Value() == "render.test");
        CHECK(failed.ErrorValue().severity == ErrorSeverity::Error);
        CHECK(failed.ErrorValue().message == "Injected native resource failure.");
        CHECK(audit->resources.creates == 0);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
        const auto retry = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(retry.HasValue());
        REQUIRE(frontend->ReleaseTransientGraphResources(retry.Value()).HasValue());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
    }

    TEST_CASE("Transient rollback metadata allocation failure retains shutdown ownership", "[renderer][transient][rollback]") {
        auto audit = std::make_shared<Audit>();
        audit->faults.failQuery = 2;
        audit->faults.queryFault = QueryFaultKind::RollbackAllocation;
        auto frontend = MakeFrontend(audit);
        auto sources = CopyGraph({{41}, 1, 1}, {{41}, 2, 1}, CopyShape::Incompatible);
        Test::RequireError(frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1}),
                           "render.frontend.resource.capacity_exhausted");
        CHECK(audit->resources.queries == 2);
        CHECK(audit->resources.creates == 0);
        CHECK(frontend->MemorySnapshot().reservationCount == 0);
        CHECK(frontend->MemorySnapshot().allocationCount == 0);
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        const auto retry = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        Test::RequireError(retry, "render.frontend.resource.backend_exception");
        CHECK(retry.ErrorValue().message.find("shut down") != std::string::npos);
        CHECK(audit->resources.queries == 2);
        frontend.reset();
        CHECK(audit->shutdowns == 1);
        CHECK(audit->resources.destroyed == 0);
    }

    TEST_CASE("Transient preparation rejects unsupported backends and malformed moved proofs", "[renderer][transient][validation]") {
        auto audit = std::make_shared<Audit>();
        auto sources = ColorGraph();
        SECTION("selected backend declines reuse") {
            audit->faults.supportsReuse = false;
            auto frontend = MakeFrontend(audit);
            Test::RequireError(frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1}), "render.frontend.resource.unsupported");
        }
        SECTION("moved lifetime proof") {
            auto intact = std::move(sources.lifetime);
            auto frontend = MakeFrontend(audit);
            REQUIRE(frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1}).HasError());
            REQUIRE(frontend->PrepareTransientGraphResources(intact, {7, 1}).HasValue());
        }
        SECTION("invalid memory scope") {
            auto frontend = MakeFrontend(audit);
            REQUIRE(frontend->PrepareTransientGraphResources(sources.lifetime, {}).HasError());
        }
        CHECK(audit->resources.creates <= 1);
    }

    TEST_CASE("Transient color execution resolves both logical resources to the same actual backing", "[renderer][transient][execution]") {
        auto audit = std::make_shared<Audit>();
        auto frontend = MakeFrontend(audit);
        auto sources = ColorGraph();
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        auto frame = BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        REQUIRE(audit->instanceCount == 2);
        CHECK(audit->instances[0].instance != 0);
        CHECK(audit->instances[0].instance == audit->instances[1].instance);
        CHECK(frontend->ReleaseTransientGraphResources(prepared.Value()).HasError());
        REQUIRE(frame.Present().HasValue());
        CHECK(audit->completionObserved);
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
    }

    TEST_CASE("Transient backing cannot be reused before actual completion", "[renderer][transient][completion]") {
        auto audit = std::make_shared<Audit>();
        audit->faults.delayCompletion = true;
        auto frontend = MakeFrontend(audit);
        auto sources = ColorGraph();
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        auto frame = BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        REQUIRE(frame.Present().HasValue());
        auto premature = BeginFrame(*frontend, 2);
        Test::RequireError(premature.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()),
                           "render.frontend.resource.not_ready");
        CHECK(audit->retained != nullptr);
        audit->Complete();
        auto completed = BeginFrame(*frontend, 3);
        REQUIRE(completed.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        REQUIRE(completed.Present().HasValue());
        audit->Complete();
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
    }

    TEST_CASE("Transient logical release retains backing until completion then drains retirement", "[renderer][transient][completion]") {
        auto audit = std::make_shared<Audit>();
        audit->faults.delayCompletion = true;
        auto frontend = MakeFrontend(audit);
        auto sources = ColorGraph();
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        auto frame = BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        REQUIRE(frame.Present().HasValue());
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        CHECK(audit->resources.destroyed == 0);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 16);
        audit->Complete();
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(audit->resources.destroyed == 1);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
    }

    TEST_CASE("Immediate command failure retains its transferred transient completion lease", "[renderer][transient][completion]") {
        auto audit = std::make_shared<Audit>();
        audit->faults.delayCompletion = true;
        audit->faults.failAfterTransfer = true;
        auto frontend = MakeFrontend(audit);
        auto sources = ColorGraph();
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        auto frame = BeginFrame(*frontend);
        Test::RequireError(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()),
                           "render.test.transient_native_failure");
        REQUIRE(frontend->ReleaseTransientGraphResources(prepared.Value()).HasValue());
        CHECK(audit->resources.destroyed == 0);
        CHECK(audit->retained != nullptr);
        audit->Complete();
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(audit->resources.destroyed == 1);
    }

    TEST_CASE("Transient set identity rejects foreign graphs renderers released sets and restart", "[renderer][transient][validation]") {
        auto audit = std::make_shared<Audit>();
        auto frontend = MakeFrontend(audit);
        auto sources = ColorGraph();
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        auto handle = prepared.Value();
        SECTION("foreign renderer") {
            ++handle.renderer.value;
        }
        SECTION("foreign graph") {
            ++handle.graph.value;
        }
        SECTION("released identity") {
            REQUIRE(frontend->ReleaseTransientGraphResources(handle).HasValue());
        }
        SECTION("malformed identity") {
            handle.value = 0;
        }
        SECTION("renderer restart") {
            frontend.reset();
            frontend = MakeFrontend(audit);
        }
        auto frame = BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, handle).HasError());
        CHECK(audit->instanceCount == 0);
    }

    TEST_CASE("Transient shutdown closes an outstanding completion lease before registry destruction", "[renderer][transient][shutdown]") {
        auto audit = std::make_shared<Audit>();
        audit->faults.delayCompletion = true;
        auto frontend = MakeFrontend(audit);
        auto sources = ColorGraph();
        const auto prepared = frontend->PrepareTransientGraphResources(sources.lifetime, {7, 1});
        REQUIRE(prepared.HasValue());
        auto frame = BeginFrame(*frontend);
        REQUIRE(frame.ExecuteGraph(sources.execution, sources.workloads, prepared.Value()).HasValue());
        frontend.reset();
        CHECK(audit->retained == nullptr);
        CHECK(audit->shutdowns == 1);
        CHECK(frame.Present().HasError());
    }
}  // namespace Horo::Render::TransientTest
