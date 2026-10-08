#include "../world_streaming/StreamingCellCandidateTestSupport.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    namespace {
        namespace W = WorldStreaming;
        using W::CandidateTestSupport::Cell;
        using W::TestSupport::Asset;
        using W::TestSupport::IdentityFrom;
        using W::TestSupport::World;

        struct Counts final {
            int authoritiesReleased{};
        };

        class Authority final : public SceneCellPayloadAuthority {
        public:
            Authority(SceneCellPayloadIdentity identity, W::StreamingFence fence, Counts &counts)
                : identity_(identity), fence_(fence), counts_(counts) {}

            ~Authority() override {
                ++counts_.authoritiesReleased;
            }

            Result<void> ValidatePublication(const SceneCellPayloadIdentity &identity, const W::StreamingFence &fence) const override {
                if (identity != identity_ || fence != fence_)
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Stale));
                return Result<void>::Success();
            }

        private:
            SceneCellPayloadIdentity identity_;
            W::StreamingFence fence_;
            Counts &counts_;
        };

        RuntimeSceneCellPayload Payload(const int cell, const std::span<const SceneAssetDependency> dependencies) {
            const auto manifest = W::CandidateTestSupport::Manifest();
            const SceneCellPayloadIdentity identity{World(), Cell(cell), {77 + static_cast<std::uint64_t>(cell)}, {1}};
            const auto object = static_cast<std::uint64_t>(10 + cell * 10);
            const std::array entities{RuntimeEntityDefinition{.object = {object + 1}, .parent = SceneObjectId{object}},
                                      RuntimeEntityDefinition{.object = {object}}};
            auto cooked =
                CookRuntimeSceneCellPayload(manifest.Descriptor(), {identity, entities, dependencies}, identity, {32, 32, 1024 * 1024});
            REQUIRE(cooked.HasValue());
            return std::move(cooked).Value();
        }

        void Activate(RuntimeSceneService &service) {
            SceneDefinitionBuilder builder{{1}, {1}};
            builder.Add({.object = {100}});
            REQUIRE(service.QueuePreparation(std::move(builder).Build().Value()).HasValue());
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, FrameContext{1, {}, 0, 0, {}, false, {}}).HasValue());
        }

        SceneCellAttachmentRequest Request(const RuntimeSceneService &service, const RuntimeSceneCellPayload &payload) {
            return {service.ActiveScene()->RuntimeId(),
                    {World(), IdentityFrom<W::PartitionEpoch>(1), payload.Identity().cell, IdentityFrom<W::StreamingGeneration>(1)},
                    {},
                    {4, 32, 32},
                    {},
                    {},
                    {}};
        }

        std::shared_ptr<Authority> Owner(const RuntimeSceneCellPayload &payload, const SceneCellAttachmentRequest &request,
                                         Counts &counts) {
            return std::make_shared<Authority>(payload.Identity(), request.fence, counts);
        }

        void Commit(RuntimeSceneService &service) {
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, FrameContext{1, {}, 0, 0, {}, false, {}}).HasValue());
        }

        struct Fixture final {
            Counts counts;
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider provider;
            JobSystem jobs{{1, 4}};
            Assets::AssetLoadService loads{jobs, provider};
            RuntimeSceneService service{registry, loads};
            SceneAssetDependency dependency{Asset(1), Assets::AssetTypeId::Parse("core.mesh").Value()};
            std::vector<Assets::AssetRecord> records{{dependency.id, dependency.expectedType, ProjectPath::Parse("assets/mesh.bin").Value(),
                                                      ProjectPath::Parse("assets/mesh.bin.horo").Value()}};
            std::shared_ptr<Assets::AssetPayloadCache> cache;
            Assets::AssetPayloadLease pin;
            Sha256Digest digest;
            RuntimeSceneCellPayload first{Payload(0, std::span{&dependency, 1})};
            RuntimeSceneCellPayload second{Payload(1, std::span{&dependency, 1})};
            SceneCellAttachmentRequest firstRequest;
            SceneCellAttachmentRequest secondRequest;

            Fixture() {
                REQUIRE(registry.Publish(records).status == Assets::AssetRegistryBuildStatus::Complete);
                Activate(service);
                Assets::AssetCookArtifact artifact;
                artifact.id = dependency.id;
                artifact.type = dependency.expectedType;
                artifact.target = AssetCookTargetId::Parse("test-host").Value();
                artifact.payload = {1, 2, 3};
                artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
                auto encoded = Assets::EncodeCookedArtifact(artifact).Value();
                cache = Assets::AssetPayloadCache::Create(4, 1024 * 1024).Value();
                pin = cache->Admit(std::as_bytes(std::span{encoded})).Value();
                digest = pin.Digest();
                firstRequest = Request(service, first);
                secondRequest = Request(service, second);
                firstRequest.registry = registry.Snapshot().Revision();
                secondRequest.registry = firstRequest.registry;
            }
        };
    }  // namespace

    TEST_CASE("Cell asset attachment rejects invalid prepared resources", "[scene][cell_attachment][assets]") {
        Fixture fixture;
        REQUIRE(QueueRuntimeSceneCellAttachment(fixture.service, fixture.first, fixture.firstRequest, {},
                                                Owner(fixture.first, fixture.firstRequest, fixture.counts))
                    .HasError());
        REQUIRE(QueueRuntimeSceneCellAttachment(fixture.service, fixture.first, fixture.firstRequest,
                                                {{{Asset(2), fixture.dependency.expectedType}, fixture.pin}},
                                                Owner(fixture.first, fixture.firstRequest, fixture.counts))
                    .HasError());
        const std::array corrupt{std::byte{1}};
        auto malformed = fixture.cache->Admit(corrupt).Value();
        REQUIRE(QueueRuntimeSceneCellAttachment(fixture.service, fixture.first, fixture.firstRequest, {{fixture.dependency, malformed}},
                                                Owner(fixture.first, fixture.firstRequest, fixture.counts))
                    .HasError());
        auto emptyView = *fixture.service.ActiveScene();
        REQUIRE(QueueRuntimeSceneCellAttachment(fixture.service, fixture.first, fixture.firstRequest, {{fixture.dependency, fixture.pin}},
                                                Owner(fixture.first, fixture.firstRequest, fixture.counts))
                    .HasValue());
        REQUIRE(emptyView.IsCurrent());
    }

    TEST_CASE("Cells share asset allocations until the final resident is retired", "[scene][cell_attachment][assets]") {
        Fixture fixture;
        REQUIRE(QueueRuntimeSceneCellAttachment(fixture.service, fixture.first, fixture.firstRequest, {{fixture.dependency, fixture.pin}},
                                                Owner(fixture.first, fixture.firstRequest, fixture.counts))
                    .HasValue());
        Commit(fixture.service);
        REQUIRE_FALSE(fixture.service.TakeOperationError().has_value());
        REQUIRE(QueueRuntimeSceneCellAttachment(fixture.service, fixture.second, fixture.secondRequest, {{fixture.dependency, fixture.pin}},
                                                Owner(fixture.second, fixture.secondRequest, fixture.counts))
                    .HasValue());
        const auto residentView = *fixture.service.ActiveScene();
        REQUIRE(fixture.registry.Publish(fixture.records).status == Assets::AssetRegistryBuildStatus::Complete);
        Commit(fixture.service);
        REQUIRE(fixture.service.TakeOperationError()->code.Value() == "scene.asset.registry_stale");
        REQUIRE(residentView.IsCurrent());
        REQUIRE(fixture.service.ActiveScene()->BaselineCount() == 1);
        fixture.secondRequest.registry = fixture.registry.Snapshot().Revision();
        REQUIRE(QueueRuntimeSceneCellAttachment(fixture.service, fixture.second, fixture.secondRequest, {{fixture.dependency, fixture.pin}},
                                                Owner(fixture.second, fixture.secondRequest, fixture.counts))
                    .HasValue());
        Commit(fixture.service);
        REQUIRE_FALSE(fixture.service.TakeOperationError().has_value());
        REQUIRE(fixture.service.ActiveScene()
                    ->FindBaseline(fixture.first.Identity().scene)
                    ->resources[0]
                    .artifact.SharesAllocationWith(
                        fixture.service.ActiveScene()->FindBaseline(fixture.second.Identity().scene)->resources[0].artifact));
        fixture.pin = {};
        fixture.cache->Evict(fixture.digest);
        REQUIRE(fixture.cache->Snapshot().retainedPayloadBytes > 0);
        const auto stale = QueueRuntimeSceneCellDetachment(fixture.service, fixture.first.Identity(), fixture.firstRequest,
                                                           Owner(fixture.first, fixture.firstRequest, fixture.counts));
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == "scene.asset.registry_stale");
        fixture.firstRequest.registry = fixture.registry.Snapshot().Revision();
        REQUIRE(QueueRuntimeSceneCellDetachment(fixture.service, fixture.first.Identity(), fixture.firstRequest,
                                                Owner(fixture.first, fixture.firstRequest, fixture.counts))
                    .HasValue());
        Commit(fixture.service);
        REQUIRE(fixture.cache->Snapshot().retainedPayloadBytes > 0);
        fixture.service.Shutdown();
        REQUIRE(fixture.cache->Snapshot().retainedPayloadBytes == 0);
    }
}  // namespace Horo::Runtime
