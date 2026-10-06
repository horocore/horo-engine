#include "SceneRestoreBundleTestHelpers.h"

namespace Horo::Runtime {
    namespace {
        using namespace RestoreTest;

        TEST_CASE("Service references use actual active module instances and retain their generation pin",
                  "[unit][runtime][scene][save][restore][references]") {
            GraphFixture fixture;
            auto owner = std::make_shared<ServiceOwner>();
            REQUIRE(owner->runtime->InstanceCount() == 1);
            const auto identity = owner->runtime->ActiveServices().front();
            auto authorities = fixture.Authorities();
            authorities.services = &owner->registry;
            authorities.activeServices = owner->runtime->ActiveServices();
            authorities.serviceGeneration = 5;
            authorities.serviceLease = owner;
            const std::array<RestoreReferenceTarget, 2> nodes{Target(1), RestoreServiceReference{identity, 5}};
            RestoreReferenceRequest reference{.identity = 1,
                                              .owner = fixture.owners[0],
                                              .source = Target(1),
                                              .target = RestoreServiceReference{identity, 5}};
            auto graph = PreparedRestoreReferenceGraph::Create(authorities, nodes, std::span{&reference, 1});
            REQUIRE(graph.HasValue());
            const auto context =
                graph.Value().MakeContext({17, fixture.fixture.participants.Generation(), authorities.scene.RuntimeId().value});
            REQUIRE(context.HasValue());
            CHECK(std::get<SaveRestoreServiceTarget>(context.Value().Results()[0].target).generation == 5);
            reference.target = RestoreServiceReference{identity, 6};
            reference.presence = RestoreReferencePresence::Optional;
            CHECK(PreparedRestoreReferenceGraph::Create(authorities, nodes, std::span{&reference, 1}).HasError());
            authorities.activeServices = {};
            CHECK(PreparedRestoreReferenceGraph::Create(authorities, nodes, std::span{&reference, 1}).HasError());
            std::weak_ptr<ServiceOwner> pin = owner;
            owner.reset();
            authorities.serviceLease.reset();
            CHECK_FALSE(pin.expired());
        }

        TEST_CASE("Asset references resolve only actual cooked Scene payloads with the exact semantic type",
                  "[unit][runtime][scene][save][restore][references]") {
            Fixture owners;
            const auto asset = Assets::AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff").Value();
            const auto type = Assets::AssetTypeId::Parse("core.mesh").Value();
            const auto path = ProjectPath::Parse("assets/reference.bin").Value();
            const auto metadata = ProjectPath::Parse("assets/reference.bin.horo").Value();
            Assets::AssetRegistry registry;
            REQUIRE(registry.Publish({{asset, type, path, metadata}}).status == Assets::AssetRegistryBuildStatus::Complete);
            Assets::MemoryAssetProvider provider;
            provider.Insert(asset, {1, 2, 3});
            JobSystem jobs{JobSystemConfig{1, 4}};
            Assets::AssetLoadService loads{jobs, provider};
            RuntimeSceneService service{registry, loads};
            PrepareCookedScene(service, owners.cancellation.Token(), asset, type);
            const auto scene = *service.ActiveScene();
            const std::array records{Entity(1)};
            const std::array bindings{PersistentEntityRuntimeBinding{Id<PersistentEntityId>(1), {1}, *scene.Find(SceneObjectId{1})}};
            const auto identities = PersistentEntityIdentityMap::Create(scene.RuntimeId(), records, bindings).Value();
            const std::array prepared{owners.adapter->Descriptor().participant.participant};
            const RestoreReferenceAuthorities authorities{.world = Id<SaveWorldId>(1),
                                                          .scene = scene,
                                                          .identities = &identities,
                                                          .participants = owners.participants,
                                                          .preparedOwners = prepared};
            const std::array<RestoreReferenceTarget, 2> nodes{Target(1), RestoreAssetReference{asset, type}};
            RestoreReferenceRequest reference{.identity = 1,
                                              .owner = prepared[0],
                                              .source = Target(1),
                                              .target = RestoreAssetReference{asset, type}};
            auto graph = PreparedRestoreReferenceGraph::Create(authorities, nodes, std::span{&reference, 1});
            REQUIRE(graph.HasValue());
            const auto context = graph.Value().MakeContext({17, owners.participants.Generation(), scene.RuntimeId().value});
            REQUIRE(context.HasValue());
            CHECK(std::get<SaveRestoreAssetTarget>(context.Value().Results()[0].target).identity.Bytes() == asset.Bytes());
            reference.target = RestoreAssetReference{asset, Assets::AssetTypeId::Parse("core.texture").Value()};
            reference.presence = RestoreReferencePresence::Optional;
            CHECK(PreparedRestoreReferenceGraph::Create(authorities, nodes, std::span{&reference, 1}).HasError());
            service.Shutdown();
            loads.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Drain);
        }

        TEST_CASE("Stable graph ordering is independent of input order and only allocation cycles fail",
                  "[unit][runtime][scene][save][restore][references]") {
            GraphFixture fixture;
            std::array<RestoreReferenceTarget, 2> nodes{Target(2), Target(1)};
            std::array
                references{RestoreReferenceRequest{.identity = 2, .owner = fixture.owners[0], .source = Target(2), .target = Target(1)},
                           RestoreReferenceRequest{.identity = 1, .owner = fixture.owners[0], .source = Target(1), .target = Target(2)}};
            auto cyclic = PreparedRestoreReferenceGraph::Create(fixture.Authorities(), nodes, references);
            REQUIRE(cyclic.HasValue());
            CHECK(cyclic.Value().References()[0].request.identity == 1);
            const auto generation = SaveRestoreReferenceGeneration{17, fixture.fixture.participants.Generation(),
                                                                   fixture.fixture.service.ActiveScene()->RuntimeId().value};
            auto context = cyclic.Value().MakeContext(generation);
            REQUIRE(context.HasValue());
            CHECK(context.Value().ForParticipant(fixture.owners[0]).references.size() == 2);
            CHECK(cyclic.Value().MakeContext({17, generation.registry, generation.candidateScene + 1}).HasError());
            for (auto &reference : references)
                reference.phase = RestoreReferencePhase::AllocationPrerequisite;
            CHECK(PreparedRestoreReferenceGraph::Create(fixture.Authorities(), nodes, references).HasError());
            references[1].phase = RestoreReferencePhase::DeferredFixup;
            auto ordered = PreparedRestoreReferenceGraph::Create(fixture.Authorities(), nodes, references);
            REQUIRE(ordered.HasValue());
            CHECK(ordered.Value().AllocationOrder()[0] == RestoreReferenceTarget{Target(1)});
            CHECK(ordered.Value().AllocationOrder()[1] == RestoreReferenceTarget{Target(2)});
            std::ranges::reverse(nodes);
            std::ranges::reverse(references);
            auto reversed = PreparedRestoreReferenceGraph::Create(fixture.Authorities(), nodes, references);
            REQUIRE(reversed.HasValue());
            CHECK(std::ranges::equal(ordered.Value().AllocationOrder(), reversed.Value().AllocationOrder()));
        }

        TEST_CASE("Optional absence and explicit remap retain exact durable generation and schema policy",
                  "[unit][runtime][scene][save][restore][references]") {
            GraphFixture fixture;
            const std::array<RestoreReferenceTarget, 2> nodes{Target(1), Target(2)};
            RestoreReferenceRequest request{.identity = 1,
                                            .owner = fixture.owners[0],
                                            .source = Target(1),
                                            .target = Target(9),
                                            .presence = RestoreReferencePresence::Optional};
            const auto build = [&] {
                return PreparedRestoreReferenceGraph::Create(fixture.Authorities(), nodes, std::span{&request, 1});
            };
            auto absent = build();
            REQUIRE(absent.HasValue());
            CHECK(absent.Value().References()[0].disposition == SaveReferenceDisposition::Missing);
            request.permittedReplacement = Target(2);
            request.presence = RestoreReferencePresence::Required;
            auto remapped = build();
            REQUIRE(remapped.HasValue());
            CHECK(remapped.Value().References()[0].disposition == SaveReferenceDisposition::Remapped);
            CHECK(remapped.Value().References()[0].entity == fixture.fixture.service.ActiveScene()->Find(SceneObjectId{2}));
            request.target = Target(2, 2);
            request.presence = RestoreReferencePresence::Optional;
            CHECK(build().HasError());
            request.target = Target(9);
            request.permittedReplacement =
                RestoreParticipantReference{fixture.owners[0], fixture.fixture.adapter->Descriptor().participant.schemaVersion, {}};
            CHECK(build().HasError());
        }

        TEST_CASE("Reference graph rejects duplicates bounds absent owner receipts and stale participant schemas",
                  "[unit][runtime][scene][save][restore][references]") {
            GraphFixture fixture;
            const std::array<RestoreReferenceTarget, 2> nodes{Target(1), Target(2)};
            RestoreReferenceRequest request{.identity = 1, .owner = fixture.owners[0], .source = Target(1), .target = Target(2)};
            const std::array duplicate{request, request};
            CHECK(PreparedRestoreReferenceGraph::Create(fixture.Authorities(), nodes, duplicate).HasError());
            CHECK(PreparedRestoreReferenceGraph::Create(fixture.Authorities(), nodes, std::span{&request, 1}, {1, 1}).HasError());
            auto authority = fixture.Authorities();
            authority.preparedOwners = {};
            CHECK(PreparedRestoreReferenceGraph::Create(authority, nodes, std::span{&request, 1}).HasError());
            request.target = RestoreParticipantReference{fixture.owners[0], Test::V<ParticipantSchemaVersion>(99), {}};
            CHECK(PreparedRestoreReferenceGraph::Create(fixture.Authorities(), nodes, std::span{&request, 1}).HasError());
            request.target = RestoreParticipantReference{fixture.owners[0], fixture.fixture.adapter->Descriptor().participant.schemaVersion,
                                                         fixture.fixture.adapter->Descriptor().record};
            REQUIRE(PreparedRestoreReferenceGraph::Create(fixture.Authorities(), nodes, std::span{&request, 1}).HasValue());
        }

    }  // namespace
}  // namespace Horo::Runtime
