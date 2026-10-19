#include "AiSceneTestSupport.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Prefab/CookedPrefab.h"

namespace Horo::AI {
    namespace {
        using namespace TestSupport;

        /** @brief Real AI owner reservation unit boundary; full provider/Scene atomic publication is covered separately. */
        struct IdentityFixture final {
            RestoreHarness resident;
            ActivationFixture added{MakeFixture(1, 11, 7, 2, 2)};
            Runtime::RuntimeComponentSet components{.aiAgent = added.agent.agent, .aiController = added.agent.controller};
            const Assets::AssetId asset{Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value()};
            std::unique_ptr<Assets::AssetPayloadCache> cache{std::move(Assets::AssetPayloadCache::Create(1, 65536)).Value()};
            std::vector<Runtime::RuntimeGroupAssetLease> resources;
            std::array<Assets::AssetId, 1> lineage{asset};
            std::unique_ptr<Runtime::SceneStructuralParticipant> participant{
                resident.runtime.MakeStructuralParticipant(std::array{resident.fixtures.front().descriptor})};

            IdentityFixture() {
                Prefab::CookedPrefabData data;
                data.assetId = asset;
                Prefab::CookedPrefabEntity root;
                root.provenance = {asset, {}, ComputeSha256(std::as_bytes(std::span("source", 6)))};
                data.entities.push_back(std::move(root));
                auto cooked = Prefab::CookedPrefab::Create(std::move(data), Prefab::PrefabLimitProfile::Create({}).Value());
                REQUIRE(cooked.HasValue());
                std::vector<std::uint8_t> payload;
                for (const auto byte : cooked.Value().Bytes())
                    payload.push_back(std::to_integer<std::uint8_t>(byte));
                const auto type = Assets::AssetTypeId::Parse("core.prefab").Value();
                const auto digest = ComputeSha256(std::as_bytes(std::span(payload)));
                auto envelope = Assets::EncodeCookedArtifact(
                    {asset, type, AssetCookTargetId::Parse("linux-x64").Value(), {}, {}, digest, std::move(payload)});
                REQUIRE(envelope.HasValue());
                auto retained = cache->Admit(std::as_bytes(std::span(envelope.Value())));
                REQUIRE(retained.HasValue());
                resources.push_back({{asset, type}, std::move(retained).Value()});
            }

            Runtime::RuntimeEntityView View(const std::uint32_t index = 2) const {
                return {.entity = {added.binding.scene, {index, 1}},
                        .components = &components,
                        .groupAssets = resources,
                        .groupSpawnLineage = lineage};
            }

            std::unique_ptr<Runtime::SceneStructuralCandidate> Prepare(const std::uint32_t index = 2) {
                auto candidate = participant->Prepare(resident.scene->View(), std::array{View(index)}, {});
                REQUIRE(candidate.HasValue());
                return std::move(candidate).Value();
            }
        };

        TEST_CASE("AI prefab reservations roll back without consuming identities or resident tasks", "[ai][prefab][identity]") {
            IdentityFixture fixture;
            REQUIRE(fixture.participant->PreparationPhase() == Runtime::SceneStructuralPreparationPhase::Identity);
            REQUIRE(fixture.resident.runtime.StartTaskAtSafePoint(fixture.resident.handle, MakeIdentity<TaskId>(31)).HasValue());
            auto candidate = fixture.Prepare();
            REQUIRE(candidate->AgentIdentities().size() == 1);
            const auto first = candidate->AgentIdentities().front();
            REQUIRE(first.original == fixture.added.agent.agent.agent);
            REQUIRE(first.resolved.Value() == 3);
            REQUIRE(fixture.components.aiAgent->agent == first.original);
            candidate.reset();
            auto retry = fixture.Prepare();
            REQUIRE(retry->AgentIdentities().front().resolved == first.resolved);
            REQUIRE(fixture.resident.runtime.Snapshot().Value().Agents().size() == 2);
            REQUIRE(fixture.resident.runtime.Find(fixture.resident.handle).Value().hasRunningTask);
        }

        TEST_CASE("AI publication fences overlapping reservations and advances repeated prefab identities", "[ai][prefab][identity]") {
            IdentityFixture fixture;
            auto first = fixture.Prepare();
            auto overlap = fixture.Prepare(3);
            REQUIRE(first->ValidatePublication().HasValue());
            first->Publish();
            REQUIRE(first->AfterPublication().HasValue());
            REQUIRE(overlap->ValidatePublication().HasError());
            overlap.reset();
            auto second = fixture.Prepare(3);
            REQUIRE(second->AgentIdentities().front().resolved.Value() == 4);
            second->Publish();
            REQUIRE(second->AfterPublication().HasValue());
            REQUIRE(fixture.resident.runtime.Snapshot().Value().Agents().size() == 4);
        }

        TEST_CASE("AI preserves ordinary authored identities and rejects prefab high-water overflow atomically", "[ai][prefab][identity]") {
            IdentityFixture fixture;
            auto ordinary = fixture.components;
            ordinary.aiAgent->agent = MakeIdentity<AgentId>(std::numeric_limits<std::uint64_t>::max());
            const Runtime::RuntimeEntityView authored{.entity = {fixture.added.binding.scene, {3, 1}}, .components = &ordinary};
            const std::array views{fixture.View(), authored};
            auto rejected = fixture.participant->Prepare(fixture.resident.scene->View(), views, {});
            REQUIRE(rejected.HasError());
            REQUIRE(fixture.resident.runtime.Snapshot().Value().Agents().size() == 2);
            REQUIRE(ordinary.aiAgent->agent.Value() == std::numeric_limits<std::uint64_t>::max());
            auto retry = fixture.Prepare();
            REQUIRE(retry->AgentIdentities().front().resolved.Value() == 3);
        }
    }  // namespace
}  // namespace Horo::AI
