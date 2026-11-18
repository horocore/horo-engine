#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderOriginScene.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace Horo::Render {
    namespace {
        namespace WS = WorldStreaming;

        template <typename T> T RequireValue(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().message : "ready"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        template <typename Id> Id Identity(std::uint64_t value) {
            return RequireValue(Id::Create(value));
        }

        /** @brief Four kilometres of valid local extent around a very distant canonical world anchor. */
        constexpr std::int64_t Anchor = 8'000'000'000'000;
        constexpr std::int64_t Travel = 2'000'000;

        Math::WorldCoordinate64 Position(std::int64_t x = 0, std::int64_t y = 0, std::int64_t z = 0) {
            return Math::WorldCoordinate64::FromMillimeters(Anchor + x, y, z);
        }

        std::unique_ptr<RenderFrontend> Frontend() {
            RenderBackendRegistry registry;
            REQUIRE(RegisterNullRenderBackend(registry).HasValue());
            REQUIRE(registry.Seal().HasValue());
            return RequireValue(RenderFrontend::Create(registry, RenderBackendId{"null"}, {}));
        }

        /** @brief Observes the exact camera/culling and GPU-facing upload inputs from the normal renderer execution path. */
        class OriginExecutor final : public IStaticMeshPassExecutor {
        public:
            Result<void> ExecuteStaticMeshPass(const StaticMeshPassDescriptor &pass) override {
                ++calls;
                camera = pass.scene.camera;
                model = pass.scene.instances.front().localToWorld;
                light = pass.scene.lights.front().position;
                return Result<void>::Success();
            }

            std::uint32_t calls{};
            RenderCameraView camera;
            Math::Mat4 model;
            Math::Vec3 light;
        };

        struct Fixture final {
            WS::OriginFrame initial =
                RequireValue(WS::OriginFrame::Create({Identity<WS::OriginFrameId>(7), Identity<WS::OriginFrameRevision>(1),
                                                      Identity<WS::OriginGeneration>(1)},
                                                     Position()));
            std::unique_ptr<WS::OriginFrameOwner> origin = RequireValue(WS::OriginFrameOwner::Create(initial));
            OriginExecutor executor;
            std::unique_ptr<RenderFrontend> frontend = Frontend();
            RenderViewId view = RequireValue(frontend->CreateRenderViewIdentity());
            WS::OriginRebaseParticipantRequirement requirement{Identity<WS::OriginRebaseParticipantId>(3),
                                                               Identity<WS::OriginRebaseParticipantRevision>(1)};
            std::unique_ptr<RenderOriginSceneAdapter> adapter =
                RequireValue(RenderOriginSceneAdapter::Create(*origin, view, requirement, {2, 1, 1, 96}));
            RenderMeshSourceHandle handle{{7}, 3};
            std::array<RenderOriginInstance, 1> instances{{{Position(Travel + 1234, 0, -4000), {handle}}}};
            std::array<RenderOriginLight, 1> lights{{{Position(Travel + 2000, 1000, -3500), {}}}};

            RenderOriginSceneSource Source() const {
                return {{Position(Travel), Position(Travel, 0, -1000)}, instances, lights, 19};
            }

            WS::OriginRebaseTransaction Rebase(WS::OriginFrame active) {
                const auto policy = RequireValue(WS::OriginShiftPolicy::Create({.id = Identity<WS::OriginShiftPolicyId>(4),
                                                                                .revision = Identity<WS::OriginShiftPolicyRevision>(1),
                                                                                .frame = active.Binding().identity,
                                                                                .mode = WS::OriginShiftRuntimeMode::EditorPreview}));
                const auto decision = RequireValue(
                    WS::EvaluateOriginShift(policy, {policy.Id(), policy.Revision(), active, WS::OriginShiftPolicyState::Active},
                                            {Identity<WS::OriginShiftRequestId>(5), WS::OriginShiftRequester::Editor, Position(Travel)}));
                REQUIRE(decision.kind == WS::OriginShiftDecisionKind::RequestShift);
                const std::array participants{static_cast<WS::IOriginRebaseParticipant *>(adapter.get())};
                const std::array required{requirement};
                return RequireValue(WS::OriginRebaseTransaction::Prepare({Identity<WS::OriginRebaseTransactionId>(6), decision, active, 1,
                                                                          WS::OriginRebaseLifecycle::Active},
                                                                         required, participants));
            }

            TemporalHistoryCompatibility History() const {
                return {.view = view,
                        .provider = {1},
                        .mode = {1},
                        .renderExtent = {32, 32},
                        .targetExtent = {32, 32},
                        .providerGeneration = 1,
                        .modeGeneration = 1,
                        .surfaceGeneration = 1,
                        .rasterGeneration = 1,
                        .colorGeneration = 1,
                        .exposureGeneration = 1,
                        .inputSchemaGeneration = 1,
                        .deviceGeneration = 1,
                        .projectionGeneration = 1,
                        .jitterGeneration = 1,
                        .motionGeneration = 1,
                        .recipeGeneration = 1};
            }
        };
    }  // namespace

    TEST_CASE("Renderer origin sends consistent view, culling and GPU data through the active frontend", "[renderer][origin]") {
        Fixture f;
        REQUIRE(f.frontend->AttachStaticMeshPassExecutor(f.executor).HasValue());
        const auto target = RequireValue(f.frontend->CreateOffscreenTarget({32, 32}));
        std::string material = "long_explicit_material_identifier_owned_by_the_captured_frame";
        f.instances.front().draw.material.value = material;
        const auto allocations = Tests::AllocationProbe::Count();
        auto retained = f.adapter->Capture(f.Source());
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(retained.HasValue());
        CHECK(after == allocations);
        material.assign(material.size(), 'x');
        const auto previousHistory = RequireValue(retained.Value().StampHistory(f.History()));
        const std::array<MeshVertex, 3> vertices{
            {{{0, 0, 0}, {0, 0, 1}, {0, 0}}, {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}}};
        constexpr std::array<std::uint32_t, 3> indices{0, 1, 2};
        const std::array resources{RenderMeshResourceView{f.handle, vertices, indices, {{0, 0, 0}, {1, 1, 0}}}};
        const auto borrowed = RequireValue(retained.Value().Scene(resources));
        REQUIRE(borrowed.IsValid());

        auto frame = RequireValue(f.frontend->BeginFrame({.frameNumber = 1, .outputExtent = {32, 32}}));
        const std::array passes{
            RenderPassDescriptor{.id = {1},
                                 .staticMesh = StaticMeshPassDescriptor{.target = target, .extent = {32, 32}, .scene = borrowed}}};
        REQUIRE(frame.Execute(passes).HasValue());
        CHECK(f.executor.calls == 1);
        CHECK(f.executor.camera.position == Math::Vec3{});
        CHECK(f.executor.camera.target.z == -1.0F);
        CHECK(f.executor.model.At(0, 3) == 1.234F);
        CHECK(f.executor.model.At(2, 3) == -4.0F);
        CHECK(f.executor.light == Math::Vec3{2, 1, -3.5F});
        REQUIRE(frame.Present().HasValue());

        auto transaction = f.Rebase(f.initial);
        REQUIRE(transaction.Commit(*f.origin, WS::OriginRebaseCommitPoint::PostSimulation, WS::OriginRebaseLifecycle::Active).HasValue());
        CHECK(retained.Value().Scene(resources).HasError());
        CHECK(retained.Value().StampHistory(f.History()).HasError());
        // The preceding frame's source remains pinned while a newer generation occupies another slot.
        CHECK(borrowed.instances.front().localToWorld == f.executor.model);
        CHECK(borrowed.instances.front().material.value == "long_explicit_material_identifier_owned_by_the_captured_frame");

        const auto replacement = RequireValue(f.adapter->Capture(f.Source()));
        const auto current = RequireValue(replacement.Scene(resources));
        CHECK(current.instances.front().localToWorld == f.executor.model);
        CHECK(current.lights.front().position == f.executor.light);
        const auto history = RequireValue(replacement.StampHistory(f.History()));
        CHECK(history.sceneOriginGeneration == previousHistory.sceneOriginGeneration + 1);
        CHECK(history.view == previousHistory.view);
        auto wrongView = f.History();
        wrongView.view = RequireValue(f.frontend->CreateRenderViewIdentity());
        CHECK(replacement.StampHistory(wrongView).HasError());
    }

    TEST_CASE("Renderer origin cancels and fences stale, invalid, over-capacity and shutdown paths", "[renderer][origin]") {
        Fixture f;
        const auto retained = RequireValue(f.adapter->Capture(f.Source()));
        const auto oldScene = RequireValue(retained.Scene({}));
        {
            auto transaction = f.Rebase(f.initial);
            CHECK(transaction.Commit(*f.origin, WS::OriginRebaseCommitPoint::PreSimulation, WS::OriginRebaseLifecycle::Active).HasError());
            transaction.Rollback();
            REQUIRE(retained.Scene({}).HasValue());
        }
        SECTION("replacement with a different world owner cannot publish renderer state") {
            auto other = RequireValue(WS::OriginFrameOwner::Create(f.initial));
            auto transaction = f.Rebase(f.initial);
            REQUIRE(transaction.Commit(*other, WS::OriginRebaseCommitPoint::PostSimulation, WS::OriginRebaseLifecycle::Active).HasValue());
            REQUIRE(retained.Scene({}).HasValue());
            CHECK(f.origin->Publishes(f.initial));
        }
        SECTION("rejected capacity and range leave retained output untouched") {
            const auto busy = RequireValue(f.adapter->Capture(f.Source()));
            const auto capacity = f.adapter->Capture(f.Source());
            REQUIRE(capacity.HasError());
            CHECK(capacity.ErrorValue().code.Value() == WS::WorldStreamingErrors::OriginRebaseCapacityExceeded.code.Value());
            CHECK(busy.Scene({}).HasValue());
        }
        SECTION("external owner replacement without a renderer receipt invalidates submissions") {
            auto abandoned = f.Rebase(f.initial);
            REQUIRE(f.origin->StageReplacement(abandoned.Event().replacementFrame).HasValue());
            REQUIRE(f.origin->PublishReplacement().HasValue());
            CHECK(f.adapter->Capture(f.Source()).HasError());
            CHECK(retained.Scene({}).HasError());
            abandoned.Rollback();
        }
        SECTION("material storage bounds fail before publishing a new frame") {
            const std::string material(97, 'x');
            f.instances.front().draw.material.value = material;
            const auto capacity = f.adapter->Capture(f.Source());
            REQUIRE(capacity.HasError());
            CHECK(capacity.ErrorValue().code.Value() == WS::WorldStreamingErrors::OriginRebaseCapacityExceeded.code.Value());
        }
        SECTION("invalid scene data cannot replace any retained output") {
            f.instances.front().draw.localToWorld.values[12] = 7;
            CHECK(f.adapter->Capture(f.Source()).HasError());
            f.instances.front().draw.localToWorld = Math::Mat4::Identity();
            f.instances.front().position = Position(9'000'000);
            const auto range = f.adapter->Capture(f.Source());
            REQUIRE(range.HasError());
            CHECK(range.ErrorValue().code.Value() == WS::WorldStreamingErrors::OriginFrameRangeExceeded.code.Value());
        }
        CHECK(oldScene.instances.front().localToWorld.At(0, 3) == 1.234F);
        f.adapter->Shutdown();
        CHECK(f.adapter->Capture(f.Source()).HasError());
        CHECK(retained.Scene({}).HasError());
        CHECK(oldScene.instances.front().localToWorld.At(0, 3) == 1.234F);
    }
}  // namespace Horo::Render
