#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabSpawnTestSupport.h"

namespace Horo::Prefab {
    using namespace SpawnTest;

    TEST_CASE("Queued prefab cancellation and module revocation preserve the original Scene", "[prefab][spawn][lifecycle]") {
        Fixture fixture;
        fixture.Warm();
        CancellationSource cancellation;
        auto operation = fixture.context->Spawn(fixture.Request(), cancellation.Token());
        REQUIRE(operation.HasValue());
        fixture.Advance();
        CHECK(fixture.Count() == 1);
        CHECK(fixture.trace.creates == 1);
        SECTION("operation cancellation") {
            operation.Value().Cancel();
        }
        SECTION("external ancestry") {
            cancellation.RequestCancellation();
        }
        SECTION("scope revocation") {
            fixture.context->Revoke();
        }
        SECTION("runner teardown revokes copied clients") {
            fixture.runner->Shutdown();
        }
        SECTION("provider shutdown closes queued admission") {
            fixture.templates.Shutdown();
        }
        fixture.Commit();
        // A closed provider may report its owner failure while the exact Scene receipt is still terminal.
        if (const auto advanced = fixture.service->Advance(fixture.tick); advanced.HasError())
            fixture.service->Shutdown();
        CHECK(operation.Value().State() != PrefabSpawnState::Committed);
        CHECK(operation.Value().Spawned().HasError());
        CHECK(fixture.Count() == 1);
        CHECK(fixture.trace.creates == 1);
    }

    TEST_CASE("Retained prefab clients and immutable results stay safe through service and Scene retirement",
              "[prefab][spawn][lifecycle]") {
        Fixture fixture;
        auto operation = fixture.context->Spawn(fixture.Request());
        REQUIRE(operation.HasValue());
        fixture.Drain(operation.Value());
        const auto instance = operation.Value().Spawned().Value();
        const auto retained = fixture.context;
        fixture.service.reset();
        fixture.templates.Shutdown();
        fixture.scenes.Shutdown();
        CHECK(operation.Value().State() == PrefabSpawnState::Committed);
        CHECK(instance.Root().IsValid());
        CHECK(retained->Spawn(fixture.Request()).HasError());
        CHECK(retained->Despawn(instance, 1).HasError());
    }

    TEST_CASE("Registry changes reject a queued prefab transaction without losing its typed receipt", "[prefab][spawn][lifecycle]") {
        Fixture fixture;
        fixture.Warm();
        auto operation = fixture.context->Spawn(fixture.Request());
        REQUIRE(operation.HasValue());
        fixture.Advance();
        REQUIRE(fixture.registry
                    .Publish({{Test::Asset(), Assets::AssetTypeId::Parse("core.prefab").Value(),
                               ProjectPath::Parse("assets/test.prefab").Value(), ProjectPath::Parse("assets/test.prefab.horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
        fixture.Commit();
        fixture.Advance();
        REQUIRE(operation.Value().State() == PrefabSpawnState::Failed);
        REQUIRE(operation.Value().Failure() != nullptr);
        CHECK(fixture.Count() == 1);
        CHECK(fixture.trace.creates == 1);
    }

    TEST_CASE("Shutdown during post-publication callbacks preserves actual committed prefab evidence", "[prefab][spawn][lifecycle]") {
        Fixture fixture;
        fixture.Warm();
        auto operation = fixture.context->Spawn(fixture.Request());
        REQUIRE(operation.HasValue());
        fixture.trace.onCreate = [&fixture](Gameplay::BehaviorContext &) {
            fixture.service->Shutdown();
        };
        fixture.Advance();
        fixture.Commit();
        CHECK(fixture.Count() == 3);
        CHECK(operation.Value().State() == PrefabSpawnState::Committed);
        CHECK(operation.Value().Spawned().HasValue());
    }

    TEST_CASE("Retained spawned behavior clients preserve recursion lineage across later ticks", "[prefab][spawn][lifecycle]") {
        Fixture fixture;
        auto operation = fixture.context->Spawn(fixture.Request());
        REQUIRE(operation.HasValue());
        fixture.Drain(operation.Value());
        bool rejected{};
        fixture.trace.onTick = [&fixture, &operation, &rejected](const Gameplay::BehaviorContext &context) {
            if (context.Entity().index == operation.Value().Spawned().Value().Root().entity.index) {
                const auto repeated = context.PrefabContext()->Spawn(fixture.Request());
                REQUIRE(repeated.HasError());
                CHECK(repeated.ErrorValue().code.Value() == PrefabErrors::SpawnRecursionDetected.code.Value());
                rejected = true;
            }
        };
        REQUIRE(fixture.runner->FixedUpdate({}, {0.01}).HasValue());
        CHECK(rejected);
        fixture.trace.onTick = {};
        REQUIRE(fixture.trace.retained != nullptr);
        CHECK(fixture.trace.retained->Spawn(fixture.Request()).HasError());
    }

    TEST_CASE("Unknown Gameplay owner data rolls back partially staged entity groups", "[prefab][spawn][lifecycle]") {
        Fixture fixture;
        auto data = fixture.Data();
        std::get<Gameplay::BehaviorComponent>(data.entities.back().members.front()).typeId =
            Gameplay::BehaviorTypeId::Parse("game.test.unregistered").Value();
        fixture.Put(std::move(data));
        auto operation = fixture.context->Spawn(fixture.Request());
        REQUIRE(operation.HasValue());
        fixture.Drain(operation.Value());
        CHECK(operation.Value().State() == PrefabSpawnState::Failed);
        CHECK(fixture.Count() == 1);
        CHECK(fixture.runner->InstanceCount() == 1);
        CHECK(fixture.trace.creates == 1);
    }

    TEST_CASE("Spawned provider schema mismatch rejects the full group before creation hooks", "[prefab][spawn][malformed]") {
        Fixture fixture;
        auto data = fixture.Data();
        auto &behavior = std::get<Gameplay::BehaviorComponent>(data.entities.back().members.front());
        SECTION("field type mismatch") {
            behavior.fields.front().value = true;
        }
        SECTION("field identity mismatch") {
            behavior.fields.front().name = "undeclared";
        }
        SECTION("schema version mismatch") {
            behavior.schemaVersion = 2;
        }
        fixture.Put(std::move(data));
        auto operation = fixture.context->Spawn(fixture.Request());
        REQUIRE(operation.HasValue());
        fixture.Drain(operation.Value());
        CHECK(operation.Value().State() == PrefabSpawnState::Failed);
        CHECK(fixture.Count() == 1);
        CHECK(fixture.trace.creates == 1);
    }

    TEST_CASE("Prefab capability grants fail closed for permission foreign scopes and non-owner lanes", "[prefab][spawn][capability]") {
        Fixture fixture;
        CHECK(fixture.service->Acquire({"game.test", fixture.scenes.ActiveScene()->RuntimeId(), true, false}).HasError());
        CHECK(fixture.service->Acquire({"game.test", {999}, true, true}).HasError());
        auto operation = fixture.context->Spawn(fixture.Request());
        REQUIRE(operation.HasValue());
        fixture.Drain(operation.Value());
        auto other = fixture.service->Acquire({"game.other", fixture.scenes.ActiveScene()->RuntimeId(), true, true});
        REQUIRE(other.HasValue());
        CHECK(other.Value()->Despawn(operation.Value().Spawned().Value(), 1).HasError());
        bool rejected{};
        std::jthread worker{[&fixture, &rejected] {
            rejected = fixture.context->Spawn(fixture.Request()).HasError();
        }};
        worker.join();
        CHECK(rejected);
    }

    TEST_CASE("Scene replacement invalidates pending prefab work and the Gameplay runner", "[prefab][spawn][lifecycle]") {
        Fixture fixture;
        auto operation = fixture.context->Spawn(fixture.Request());
        REQUIRE(operation.HasValue());
        Runtime::SceneDefinitionBuilder builder{{8}, {1}};
        REQUIRE(fixture.scenes.QueuePreparation(std::move(builder).Build().Value()).HasValue());
        fixture.Commit();
        CHECK(fixture.service->Advance(1).HasError());
        CHECK(operation.Value().State() == PrefabSpawnState::Failed);
        CHECK(fixture.context->Spawn(fixture.Request()).HasError());
        CHECK(fixture.runner->FixedUpdate({}, {0.01}).HasError());
        CHECK(fixture.Count() == 0);
    }

    TEST_CASE("Catalog republication does not prevent retirement of an already owned prefab group", "[prefab][spawn][lifecycle]") {
        Fixture fixture;
        auto operation = fixture.context->Spawn(fixture.Request());
        REQUIRE(operation.HasValue());
        fixture.Drain(operation.Value());
        REQUIRE(operation.Value().State() == PrefabSpawnState::Committed);
        REQUIRE(fixture.registry
                    .Publish({{Test::Asset(), Assets::AssetTypeId::Parse("core.prefab").Value(),
                               ProjectPath::Parse("assets/test.prefab").Value(), ProjectPath::Parse("assets/test.prefab.horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
        auto retired = fixture.context->Despawn(operation.Value().Spawned().Value(), 1);
        REQUIRE(retired.HasValue());
        fixture.Drain(retired.Value());
        CHECK(retired.Value().State() == PrefabSpawnState::Committed);
        CHECK(fixture.Count() == 1);
    }

    TEST_CASE("External prefab bindings and local member references remain typed and generation checked", "[prefab][spawn][references]") {
        Fixture fixture;
        auto data = fixture.Data();
        data.bindings = {{Property(21), {}, true}, {Property(22), {}, false}};
        data.references = {{{{0}, 0}, Property(1), CookedPrefabEntitySlot{1}},
                           {{{0}, 0}, Property(2), CookedPrefabMemberSlot{{1}, 0}},
                           {{{0}, 0}, Property(3), CookedPrefabBindingSlot{0}},
                           {{{0}, 0}, Property(4), CookedPrefabBindingSlot{1}}};
        fixture.Put(std::move(data));
        const auto original = fixture.scenes.ActiveScene()->Find({1}).value();
        auto request = fixture.Request();
        request.bindings.emplace_back(Property(21), original);
        auto operation = fixture.context->Spawn(request);
        REQUIRE(operation.HasValue());
        fixture.Drain(operation.Value());
        REQUIRE(operation.Value().State() == PrefabSpawnState::Committed);
        const auto instance = operation.Value().Spawned().Value();
        const auto root = fixture.scenes.ActiveScene()->Get(instance.Root()).Value();
        const Runtime::RuntimeGroupMemberIdentity owner{Type(), root.components->behaviors.front().instanceId.value};
        auto local = fixture.context->Reference(instance.Root(), owner, Property(1));
        auto member = fixture.context->Reference(instance.Root(), owner, Property(2));
        auto external = fixture.context->Reference(instance.Root(), owner, Property(3));
        auto optional = fixture.context->Reference(instance.Root(), owner, Property(4));
        REQUIRE(local.HasValue());
        REQUIRE(member.HasValue());
        REQUIRE(external.HasValue());
        REQUIRE(optional.HasValue());
        CHECK(std::get<Runtime::RuntimeGroupMemberReference>(member.Value().target).entity ==
              std::get<Runtime::EntityRef>(local.Value().target));
        CHECK(std::get<Runtime::RuntimeGroupExternalReference>(external.Value().target).entity == original);
        CHECK(std::holds_alternative<std::monostate>(optional.Value().target));
        Runtime::SceneCommandBuffer retire;
        retire.Destroy(original);
        const auto queued = fixture.scenes.QueueStructuralCommands(std::move(retire));
        REQUIRE(queued.HasValue());
        fixture.Commit();
        CHECK(fixture.context->Reference(instance.Root(), owner, Property(3)).HasError());
        CHECK(fixture.context->Reference(instance.Root(), owner, Property(1)).HasValue());
    }

    TEST_CASE("Missing required and invalid supplied optional prefab bindings reject complete creation", "[prefab][spawn][references]") {
        Fixture fixture;
        auto data = fixture.Data();
        data.bindings = {{Property(21), {}, true}, {Property(22), {}, false}};
        data.references = {{{{0}, 0}, Property(1), CookedPrefabBindingSlot{0}}};
        fixture.Put(std::move(data));
        auto request = fixture.Request();
        SECTION("missing required") {
            // Keep all required binding inputs absent.
        }
        SECTION("undeclared optional input") {
            request.bindings.emplace_back(Property(23), fixture.scenes.ActiveScene()->Find({1}).value());
        }
        auto operation = fixture.context->Spawn(request);
        REQUIRE(operation.HasValue());
        fixture.Drain(operation.Value());
        CHECK(operation.Value().State() == PrefabSpawnState::Failed);
        CHECK(fixture.Count() == 1);
    }
}  // namespace Horo::Prefab
