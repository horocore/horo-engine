#include "Horo/Runtime/Render/UiPaintKernel.h"
#include "RenderGraphResourceLeasePool.h"
#include "UiSubmissionSourceFixture.h"
#include "support/AllocationProbe.h"

#include <iostream>

namespace Horo::Render::Detail {
    namespace {
        using namespace Test;

        class RetainedKernel final : public IResidentUiPaintKernel {
        public:
            explicit RetainedKernel(std::size_t &destroyed) : destroyed_(destroyed) {}

            ~RetainedKernel() override {
                ++destroyed_;
            }

        private:
            std::size_t &destroyed_;
        };

        struct PaintLeaseFixture final {
            std::size_t destroyed{};
            RenderResourceRegistry registry{{1},
                                            {.maximumSubmissionPins = 8},
                                            [this](RenderResourceClass, std::uint64_t, std::optional<RenderMemoryAllocationId>,
                                                   BackendResourceReleaseMode) {
                ++destroyed;
            }};
            RenderGraphResourceLeasePool pool{registry, 8};
            Runtime::Ui::UiRenderGeometryArena arena{
                UiRequire(Runtime::Ui::UiRenderGeometryArena::Create({{UiOwner(), 9, 1}, {4, 6, 1}, 1}))};
            std::optional<Runtime::Ui::UiRenderGeometryPlan> geometry;

            PaintLeaseFixture() {
                using namespace Runtime::Ui;
                auto tree = UiTree();
                const UiRenderSnapshotLimits limits{1, 0, 0, 0, 0, 1, 0};
                auto extractor = UiRequire(UiRenderExtractor::Create({{UiOwner(), 9, 1}, limits, 1}));
                const std::array commands{UiDrawCommand{UiRequire(tree.Root()).handle,
                                                        {{0, 0}, {64, 64}},
                                                        0,
                                                        NoUiRenderIndex,
                                                        NoUiRenderIndex,
                                                        1.0F,
                                                        UiSolidDraw{{1, 1, 1, 1}}}};
                const std::array transforms{UiLogicalTransform{}};
                const auto descriptor =
                    Runtime::Ui::Test::SnapshotDescriptor(tree, UiRequire(UiInteractionRevision::Create(8)),
                                                          UiRequire(UiRenderSnapshotRevision::Create(1)), {UiOwner(), 9, 1}, limits);
                const auto snapshot = UiRequire(extractor.Extract(tree, descriptor, {.commands = commands, .transforms = transforms}));
                geometry = UiRequire(arena.Build(snapshot));
            }

            RenderGraphResource Buffer(std::uint32_t slot) {
                const auto reserved = UiRequire(registry.Reserve(RenderResourceClass::Buffer));
                REQUIRE(registry.Publish(RenderResourceClass::Buffer, reserved.identity, 40 + slot).HasValue());
                const RenderBufferHandle handle{reserved.identity.owner, reserved.identity.slot, reserved.identity.generation};
                return {{{1}, slot}, RenderGraphResourceKind::Buffer, RenderGraphResourceClass::Persistent, handle};
            }

            UiRenderSubmission Submission() {
                return {&*geometry, {}, {}, {}};
            }

            void DropGeometry() {
                geometry.reset();
                arena.Close();
            }
        };

        TEST_CASE("Canonical UI capture and later GUI share one frame completion lease", "[renderer][runtime_ui][paint][resource]") {
            std::size_t destroyedKernels{};
            PaintLeaseFixture fixture;
            const auto scene = fixture.Buffer(1);
            const auto lease = UiRequire(fixture.pool.Acquire(std::array{scene}));
            auto ui = fixture.Submission();
            std::array workloads{
                RenderGraphPassWorkload{{}, RenderGraphUiPaint{.kernel = std::make_shared<RetainedKernel>(destroyedKernels)}}};
            const auto captured = UiRequire(fixture.pool.AcquireUi({}, ui, nullptr, lease, workloads));
            auto retainedGeometry = std::move(*fixture.geometry);
            auto movedSource = fixture.Submission();
            REQUIRE_FALSE(movedSource.geometry->IsValid());
            REQUIRE(fixture.pool.AcquireUi({}, movedSource, nullptr, lease, workloads).HasError());
            fixture.geometry = std::move(retainedGeometry);
            workloads[0].workload = std::monostate{};
            REQUIRE(destroyedKernels == 0);
            REQUIRE(captured == lease);
            REQUIRE(fixture.pool.AcquireUi({}, ui, nullptr, lease).HasError());
            const auto gui = UiRequire(fixture.pool.AcquireGui({}, lease));
            REQUIRE(gui == lease);
            fixture.DropGeometry();
            REQUIRE_FALSE(fixture.arena.IsDrained());
            lease->Release();
            REQUIRE(fixture.arena.IsDrained());
            REQUIRE(destroyedKernels == 1);
        }

        TEST_CASE("One canonical completion lease retains all configured UI sources without frame allocation",
                  "[renderer][runtime_ui][paint][resource][batch]") {
            std::size_t destroyedKernels{};
            PaintLeaseFixture fixture;
            RenderGraphResourceLeasePool pool{fixture.registry, 8, 64};
            CHECK(pool.UiRetentionStorageBytes() == fixture.pool.UiRetentionStorageBytes() * 8);
            std::cout << "UiCapture prepared bytes: default8x8=" << fixture.pool.UiRetentionStorageBytes()
                      << " explicit64x8=" << pool.UiRetentionStorageBytes() << '\n';
            const auto scene = fixture.Buffer(1);
            auto *lease = UiRequire(pool.Acquire(std::array{scene}));
            std::array<std::optional<Runtime::Ui::UiRenderGeometryArena>, 65> arenas;
            std::array<std::optional<Runtime::Ui::UiRenderGeometryPlan>, 65> plans;
            for (std::size_t index = 0; index < plans.size(); ++index) {
                arenas[index].emplace(UiRequire(Runtime::Ui::UiRenderGeometryArena::Create({{UiOwner(), 9, 1}, {4, 6, 1}, 1})));
                plans[index].emplace(UiRequire(arenas[index]->Build(fixture.geometry->SourceSnapshot())));
            }
            std::array workloads{
                RenderGraphPassWorkload{{}, RenderGraphUiPaint{.kernel = std::make_shared<RetainedKernel>(destroyedKernels)}}};
            Horo::Tests::AllocationProbe::Measurement measured;
            bool capturedAll = true;
            {
                Horo::Tests::AllocationProbe::ScopedMeasurement probe;
                for (std::size_t index = 0; index < 64; ++index) {
                    UiRenderSubmission submission{&*plans[index], {}, {}, {}};
                    auto captured = pool.AcquireUi({}, submission, nullptr, lease, workloads);
                    capturedAll = capturedAll && captured.HasValue() && captured.Value() == lease;
                }
                measured = probe.Snapshot();
            }
            REQUIRE(capturedAll);
            CHECK(measured.requests == 0);
            UiRenderSubmission overflow{&*plans.back(), {}, {}, {}};
            CHECK(pool.AcquireUi({}, overflow, nullptr, lease, workloads).HasError());
            workloads[0].workload = std::monostate{};
            CHECK(destroyedKernels == 0);
            for (std::size_t index = 0; index < plans.size(); ++index) {
                plans[index].reset();
                arenas[index]->Close();
                CHECK(arenas[index]->IsDrained() == (index == 64));
            }
            REQUIRE(UiRequire(pool.AcquireGui({}, lease)) == lease);
            lease->Release();
            CHECK(destroyedKernels == 1);
            for (auto &arena : arenas)
                CHECK(arena->IsDrained());
        }

        TEST_CASE("Rejected UI capture rolls back added pins while retaining submitted scene ownership",
                  "[renderer][runtime_ui][paint][resource]") {
            PaintLeaseFixture fixture;
            const auto scene = fixture.Buffer(1);
            const auto added = fixture.Buffer(2);
            const auto lease = UiRequire(fixture.pool.Acquire(std::array{scene}));
            auto stale = added;
            ++std::get<RenderBufferHandle>(stale.binding).generation;
            auto ui = fixture.Submission();
            REQUIRE(fixture.pool.AcquireUi(std::array{added, stale}, ui, nullptr, lease).HasError());
            const auto addedHandle = std::get<RenderBufferHandle>(added.binding);
            REQUIRE(fixture.registry.Release(RenderResourceClass::Buffer, {addedHandle.owner, addedHandle.slot, addedHandle.generation})
                        .HasValue());
            REQUIRE(fixture.registry.DrainRetirements() == 1);
            REQUIRE(fixture.destroyed == 1);
            REQUIRE(UiRequire(fixture.pool.AcquireGui({}, lease)) == lease);
            fixture.DropGeometry();
            REQUIRE(fixture.arena.IsDrained());
            const auto sceneHandle = std::get<RenderBufferHandle>(scene.binding);
            REQUIRE(fixture.registry.Release(RenderResourceClass::Buffer, {sceneHandle.owner, sceneHandle.slot, sceneHandle.generation})
                        .HasValue());
            REQUIRE(fixture.registry.DrainRetirements() == 0);
            lease->Release();
            REQUIRE(fixture.registry.DrainRetirements() == 1);
        }
    }  // namespace
}  // namespace Horo::Render::Detail
