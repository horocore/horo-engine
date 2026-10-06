#include "AllocationProbe.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "Horo/Runtime/Scene/SavedSceneBootstrap.h"
#include "SaveContentTestHelpers.h"
#include "SceneTestIdentity.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Runtime {
    namespace {
        using SceneTest::Id;

        Assets::AssetTypeId SceneAssetType(const std::string_view value = "core.scene") {
            return Assets::AssetTypeId::Parse(value).Value();
        }

        Sha256Digest Digest(const std::uint8_t marker) {
            Sha256Digest digest;
            digest.bytes.back() = marker;
            return digest;
        }

        using SceneContentTest::Definition;

        /** @brief Keeps allocation rejection armed across the actual foreign callback and its typed boundary translation. */
        class FaultDecoder final : public ISavedSceneBaselineDecoder {
        public:
            enum class Fault {
                Allocation,
                Standard,
                NonStandard
            };
            Fault fault{Fault::Allocation};
            std::optional<Tests::AllocationProbe::ScopedFailure> rejection;
            std::runtime_error standardFailure{"Foreign fixture failure"};
            std::size_t allocationCount{};
            std::size_t calls{};

            Result<RuntimeSceneDefinition> DecodeBaseline(const SavedSceneBaselineDecodeInput &) override {
                ++calls;
                rejection.emplace();
                allocationCount = Tests::AllocationProbe::Count();
                if (fault == Fault::Allocation)
                    throw std::bad_alloc{};
                if (fault == Fault::Standard)
                    throw standardFailure;
                throw 42;
            }
        };

        SavedSceneBootstrapDescriptor Descriptor() {
            return {.world = Id<SaveWorldId>(1),
                    .baseScene = Id<SaveBaseSceneId>(2),
                    .expectedAssetType = SceneAssetType(),
                    .definition = SceneDefinitionId{7},
                    .revision = SceneDefinitionRevision{3},
                    .contentDigest = Digest(4),
                    .spawnAnchor = SceneObjectId{20},
                    .transition = {.slot = Id<SaveGameSlotId>(5), .generation = Id<SlotGenerationId>(6), .priorWorld = Id<SaveWorldId>(7)}};
        }

        FrameContext Context(const CancellationToken &token) {
            return FrameContext{1, {}, 0.0, 0, {}, false, token};
        }

        void RequireCode(const auto &result, const std::string_view code) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == code);
        }

        TEST_CASE("Saved scene bootstrap validates every durable identity and content requirement", "[runtime][scene][save_bootstrap]") {
            auto descriptor = Descriptor();
            REQUIRE(descriptor.IsValid());

            descriptor.world = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.baseScene = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.expectedAssetType = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.definition = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.revision = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.contentDigest = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.spawnAnchor = SceneObjectId{};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.transition.slot = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.transition.generation = {};
            REQUIRE_FALSE(descriptor.IsValid());
            descriptor = Descriptor();
            descriptor.transition.priorWorld = SaveWorldId{};
            REQUIRE_FALSE(descriptor.IsValid());

            descriptor = Descriptor();
            descriptor.spawnAnchor.reset();
            descriptor.transition.priorWorld.reset();
            REQUIRE(descriptor.IsValid());
        }

        TEST_CASE("Saved scene bootstrap resolves the actual cooked provider envelope and owns deterministic defaults",
                  "[runtime][scene][save_bootstrap]") {
            SceneContentTest::Fixture fixture;
            auto prepared = fixture.Prepare();
            REQUIRE(prepared.HasValue());
            CHECK(fixture.decoder.calls == 1);
            CHECK(fixture.decoder.observed == Assets::AssetId::FromBytes(fixture.descriptor.baseScene.Bytes()));
            CHECK(prepared.Value().BaseSceneAsset() == *fixture.decoder.observed);
            CHECK(prepared.Value().InstalledGeneration() == 1);
            CHECK(prepared.Value().Descriptor() == fixture.descriptor);
            CHECK(prepared.Value().Definition().Id() == fixture.descriptor.definition);
            auto first = RuntimeScene::Create(prepared.Value().Definition(), SceneRuntimeId{1});
            auto second = RuntimeScene::Create(prepared.Value().Definition(), SceneRuntimeId{2});
            REQUIRE(first.HasValue());
            REQUIRE(second.HasValue());
            CHECK(first.Value()->View().Find(*fixture.descriptor.spawnAnchor));
            CHECK(second.Value()->View().Find(*fixture.descriptor.spawnAnchor));
            CHECK(first.Value()->View().SlotCount() == second.Value()->View().SlotCount());
        }

        TEST_CASE("Unavailable incompatible and unsupported cooked baselines fail before any world is queued",
                  "[runtime][scene][save_bootstrap]") {
            SceneContentTest::Fixture unmounted{false};
            RequireCode(unmounted.Prepare(), "scene.save_bootstrap.asset_unavailable");
            CHECK(unmounted.decoder.calls == 0);
            SceneContentTest::Fixture wrongType{true, SceneAssetType("core.prefab")};
            RequireCode(wrongType.Prepare(), "scene.save_bootstrap.asset_unavailable");
            CHECK(wrongType.decoder.calls == 0);
            SceneContentTest::Fixture fixture;
            RequireCode(fixture.Prepare(fixture.descriptor, {}), "scene.save_bootstrap.invalid");
            auto selfApproved = fixture.descriptor;
            selfApproved.expectedAssetType = SceneAssetType("core.prefab");
            RequireCode(fixture.Prepare(selfApproved), "scene.save_bootstrap.invalid");
            auto wrongDigest = fixture.descriptor;
            wrongDigest.contentDigest = Digest(9);
            RequireCode(fixture.Prepare(wrongDigest), "scene.save_bootstrap.incompatible");
            CHECK(fixture.decoder.calls == 0);
            auto proof = fixture.Reconcile();
            REQUIRE(proof.HasValue());
            RequireCode(PrepareSavedSceneBootstrap(fixture.descriptor, SceneAssetType(), std::move(proof).Value(), nullptr),
                        "scene.save_bootstrap.decoder_unavailable");
            CHECK(fixture.decoder.calls == 0);
            SceneContentTest::Fixture wrongDefinition{true, SceneAssetType(), {8, 3}};
            RequireCode(wrongDefinition.Prepare(), "scene.save_bootstrap.incompatible");
            CHECK(wrongDefinition.decoder.calls == 1);
            SceneContentTest::Fixture wrongRevision{true, SceneAssetType(), {7, 4}};
            RequireCode(wrongRevision.Prepare(), "scene.save_bootstrap.incompatible");
            auto missingSpawn = fixture.descriptor;
            missingSpawn.spawnAnchor = SceneObjectId{99};
            RequireCode(fixture.Prepare(missingSpawn), "scene.save_bootstrap.spawn_missing");
        }

        TEST_CASE("Foreign baseline decoder faults use owned typed storage without allocation or world publication",
                  "[runtime][scene][save_bootstrap][allocation][foreign]") {
            SceneContentTest::Fixture fixture;
            auto service = std::make_shared<RuntimeSceneService>();
            REQUIRE(service->Startup(fixture.cancellation.Token()).HasValue());
            REQUIRE(service->QueuePreparation(Definition(42, 1)).HasValue());
            REQUIRE(service->OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(fixture.cancellation.Token())).HasValue());
            const auto scene = service->ActiveScene()->RuntimeId();
            FaultDecoder decoder;
            SECTION("allocation failure") {
                decoder.fault = FaultDecoder::Fault::Allocation;
            }
            SECTION("standard foreign failure") {
                decoder.fault = FaultDecoder::Fault::Standard;
            }
            SECTION("nonstandard foreign failure") {
                decoder.fault = FaultDecoder::Fault::NonStandard;
            }
            auto proof = fixture.Reconcile();
            REQUIRE(proof.HasValue());
            auto prepared = PrepareSavedSceneBootstrap(fixture.descriptor, SceneAssetType(), std::move(proof).Value(), &decoder);
            const auto allocations = Tests::AllocationProbe::Count();
            decoder.rejection.reset();
            REQUIRE(prepared.HasError());
            CHECK(allocations == decoder.allocationCount);
            CHECK(decoder.calls == 1);
            CHECK(prepared.ErrorValue().code.Value() == (decoder.fault == FaultDecoder::Fault::Allocation
                                                             ? SaveErrors::RestoreAllocationFailed.code.Value()
                                                             : SaveErrors::RestoreAdapterContractInvalid.code.Value()));
            CHECK(service->ActiveScene()->RuntimeId() == scene);
            CHECK(fixture.Prepare().HasValue());
        }

        TEST_CASE("Malformed and stale restore evidence preserves the active scene and exact publication ownership",
                  "[runtime][scene][save_bootstrap]") {
            SceneContentTest::Fixture fixture;
            auto service = std::make_shared<RuntimeSceneService>();
            REQUIRE(service->Startup(fixture.cancellation.Token()).HasValue());
            REQUIRE(service->QueuePreparation(Definition(42, 1)).HasValue());
            REQUIRE(service->OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(fixture.cancellation.Token())).HasValue());
            const auto old = service->ActiveScene()->RuntimeId();
            auto malformed = fixture.descriptor;
            malformed.transition.generation = {};
            RequireCode(fixture.Prepare(malformed), "scene.save_bootstrap.invalid");
            CHECK(service->ActiveScene()->RuntimeId() == old);
            auto prepared = fixture.Prepare();
            REQUIRE(prepared.HasValue());
            auto proof = std::move(prepared).Value();
            RequireCode(std::move(prepared).Value().Queue(service), "scene.save_bootstrap.invalid");
            auto queuedResult = std::move(proof).Queue(service);
            REQUIRE(queuedResult.HasValue());
            auto queued = std::move(queuedResult).Value();
            CHECK(service->ActiveScene()->RuntimeId() == old);
            RequireCode(std::move(proof).Queue(service), "scene.save_bootstrap.invalid");
            RequireCode(queued.BindPublishedWorld(), "scene.operation.in_progress");
            REQUIRE(service->OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(fixture.cancellation.Token())).HasValue());
            CHECK(service->ActiveScene()->DefinitionId() == fixture.descriptor.definition);
            auto world = queued.BindPublishedWorld();
            REQUIRE(world.HasValue());
            RequireCode(queued.BindPublishedWorld(), "scene.save_bootstrap.invalid");
            service->Shutdown();
        }

        TEST_CASE("An ordinary replacement with identical definition cannot bind the prior saved-world receipt",
                  "[runtime][scene][save_bootstrap]") {
            SceneContentTest::Fixture fixture;
            auto service = std::make_shared<RuntimeSceneService>();
            REQUIRE(service->Startup(fixture.cancellation.Token()).HasValue());
            auto prepared = fixture.Prepare();
            REQUIRE(prepared.HasValue());
            auto queuedResult = std::move(prepared).Value().Queue(service);
            REQUIRE(queuedResult.HasValue());
            auto queued = std::move(queuedResult).Value();
            REQUIRE(service->OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(fixture.cancellation.Token())).HasValue());
            const auto published = service->ActiveScene()->RuntimeId();
            REQUIRE(service->QueuePreparation(Definition()).HasValue());
            REQUIRE(service->OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Context(fixture.cancellation.Token())).HasValue());
            REQUIRE(service->ActiveScene()->RuntimeId() != published);
            RequireCode(queued.BindPublishedWorld(), "save.restore.activation_stale");
            service->Shutdown();
        }
    }  // namespace
}  // namespace Horo::Runtime
