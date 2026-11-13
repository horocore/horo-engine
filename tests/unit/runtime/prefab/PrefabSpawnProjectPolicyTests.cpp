#include "PrefabSpawnTestSupport.h"

namespace Horo::Prefab {
    namespace {
        using namespace SpawnTest;

        /** @brief Publish actual distinct runtime artifacts so lineage limits cannot be confused with recursion rejection. */
        std::vector<PrefabSpawnRequest> PublishChain(Fixture &fixture, const std::size_t count) {
            std::vector<Assets::AssetRecord> records;
            std::vector<PrefabSpawnRequest> requests;
            for (std::size_t index = 0; index < count; ++index) {
                const auto asset = Test::Asset(static_cast<std::uint16_t>(index + 1));
                const auto name = "assets/policy" + std::to_string(index) + ".prefab";
                records.push_back({asset, Assets::AssetTypeId::Parse("core.prefab").Value(), ProjectPath::Parse(name).Value(),
                                   ProjectPath::Parse(name + ".horo").Value()});
                auto data = fixture.Data();
                data.assetId = asset;
                for (auto &entity : data.entities)
                    entity.provenance.sourceAsset = asset;
                const auto cooked = CookedPrefab::Create(std::move(data), PrefabLimitProfile::Create({}).Value());
                REQUIRE(cooked.HasValue());
                std::vector<std::uint8_t> payload;
                for (const auto byte : cooked.Value().Bytes())
                    payload.push_back(std::to_integer<std::uint8_t>(byte));
                const auto hash = ComputeSha256(std::as_bytes(std::span(payload)));
                const auto envelope = Assets::EncodeCookedArtifact({asset,
                                                                    Assets::AssetTypeId::Parse("core.prefab").Value(),
                                                                    AssetCookTargetId::Parse("linux-x64").Value(),
                                                                    {},
                                                                    {},
                                                                    hash,
                                                                    std::move(payload)});
                REQUIRE(envelope.HasValue());
                auto request = fixture.Request();
                request.source.asset = asset;
                request.source.artifactDigest = ComputeSha256(std::as_bytes(std::span(envelope.Value())));
                fixture.bytes.Insert(asset, envelope.Value());
                requests.push_back(std::move(request));
            }
            REQUIRE(fixture.registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
            return requests;
        }
    }  // namespace

    TEST_CASE("Runtime prefab spawn uses captured project depth for exact and one-over distinct-asset lineage",
              "[prefab][spawn][settings][boundary]") {
        for (const auto maximum : {std::size_t{1}, std::size_t{2}, PrefabHardLimits::RuntimeSpawnDepth}) {
            CAPTURE(maximum);
            PrefabProjectPolicy policy;
            policy.maximumRuntimeSpawnDepth = maximum;
            Fixture fixture{2, policy};
            CHECK(fixture.templates.LimitProfile().Policy().maximumRuntimeSpawnDepth == maximum);
            const auto requests = PublishChain(fixture, maximum + 1);
            std::shared_ptr<const Gameplay::GameplayPrefabContext> context = fixture.context;
            for (std::size_t depth = 0; depth < maximum; ++depth) {
                auto operation = context->Spawn(requests[depth]);
                REQUIRE(operation.HasValue());
                fixture.Drain(operation.Value());
                REQUIRE(operation.Value().State() == PrefabSpawnState::Committed);
                const auto root = operation.Value().Spawned().Value().Root();
                std::shared_ptr<const Gameplay::GameplayPrefabContext> inherited;
                fixture.trace.onTick = [&](Gameplay::BehaviorContext &behavior) {
                    if (behavior.Entity().index == root.entity.index)
                        inherited = behavior.PrefabContext();
                };
                REQUIRE(fixture.runner->FixedUpdate({}, {0.01}).HasValue());
                fixture.trace.onTick = {};
                REQUIRE(inherited);
                context = std::move(inherited);
            }
            const auto count = fixture.Count();
            const auto rejected = context->Spawn(requests[maximum]);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::SpawnDepthExceeded.code.Value());
            CHECK(fixture.Count() == count);
        }
    }

    TEST_CASE("Runtime prefab instance bindings honor lowered and hard project ceilings before queuing",
              "[prefab][spawn][settings][boundary]") {
        for (const auto maximum : {std::size_t{1}, std::size_t{2}, PrefabHardLimits::InstanceBindings}) {
            CAPTURE(maximum);
            PrefabProjectPolicy policy;
            policy.maximumInstanceBindings = maximum;
            Fixture fixture{2, policy};
            auto data = fixture.Data();
            for (std::size_t index = 0; index < std::min(maximum + 1, PrefabHardLimits::BindingSlots); ++index) {
                data.bindings.push_back({Property(21 + index), {}, index == 0});
                data.references.push_back({{{0}, 0}, Property(200 + index), CookedPrefabBindingSlot{static_cast<std::uint32_t>(index)}});
            }
            fixture.Put(std::move(data));
            auto request = fixture.Request();
            const auto target = fixture.scenes.ActiveScene()->Find({1}).value();
            for (std::size_t index = 0; index < maximum; ++index)
                request.bindings.emplace_back(Property(21 + index), target);
            auto accepted = fixture.context->Spawn(request);
            REQUIRE(accepted.HasValue());
            fixture.Drain(accepted.Value());
            CHECK(accepted.Value().State() == PrefabSpawnState::Committed);
            request.bindings.emplace_back(Property(21 + maximum), target);
            const auto count = fixture.Count();
            const auto rejected = fixture.context->Spawn(request);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::AdmissionRejected.code.Value());
            CHECK(fixture.Count() == count);
        }
    }

    TEST_CASE("Prepared prefab groups cannot bypass their captured project lineage ceiling", "[prefab][spawn][settings][boundary]") {
        PrefabProjectPolicy policy;
        policy.maximumRuntimeSpawnDepth = 1;
        Fixture fixture{2, policy};
        fixture.Warm();
        const auto handle = fixture.templates.LoadAsync(fixture.Request().source);
        REQUIRE(handle.HasValue());
        const auto lease = fixture.templates.TakeResult(handle.Value());
        REQUIRE(lease.HasValue());
        const auto count = fixture.Count();
        PrefabPreparedGroupOptions options;
        options.spawnLineage = {Test::Asset(1), Test::Asset(2)};
        const auto rejected = fixture.templates.QueuePreparedGroup(lease.Value(), {}, {}, std::move(options));
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::SpawnDepthExceeded.code.Value());
        CHECK(fixture.Count() == count);
    }
}  // namespace Horo::Prefab
