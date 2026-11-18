#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabSpawnTestSupport.h"

#include <limits>

namespace Horo::Prefab {
    using namespace SpawnTest;

    TEST_CASE("Returned prefab fixture frames retain a live host cancellation borrow across later statements",
              "[prefab][spawn][lifetime]") {
        Fixture fixture;
        const auto retained = fixture.Frame();
        REQUIRE(fixture.scenes.ActiveScene().has_value());
        const auto later = fixture.Frame();
        REQUIRE(&retained.cancellation == &fixture.cancellationToken);
        REQUIRE(&later.cancellation == &fixture.cancellationToken);
        REQUIRE_FALSE(retained.cancellation.IsCancellationRequested());
        REQUIRE_FALSE(later.cancellation.IsCancellationRequested());
        fixture.cancellation.RequestCancellation();
        CHECK(retained.cancellation.IsCancellationRequested());
        CHECK(later.cancellation.IsCancellationRequested());
    }

    TEST_CASE("Gameplay prefab capability commits complete typed groups and despawns through the Scene owner",
              "[prefab][spawn][gameplay]") {
        Fixture fixture;
        const auto original = fixture.scenes.ActiveScene()->Find({1}).value();
        auto request = fixture.Request();
        request.placement.translation = {7, 8, 9};
        request.parent = original;
        std::optional<PrefabOperation> operation;
        bool submitted{};
        fixture.trace.onTick = [&submitted, &operation, &request](const Gameplay::BehaviorContext &context) {
            if (submitted)
                return;
            submitted = true;
            REQUIRE(context.PrefabContext() != nullptr);
            auto spawned = context.PrefabContext()->Spawn(request);
            REQUIRE(spawned.HasValue());
            operation = std::move(spawned).Value();
        };
        REQUIRE(fixture.runner->FixedUpdate({}, {0.01}).HasValue());
        REQUIRE(operation.has_value());
        CHECK(fixture.Count() == 1);
        CHECK(fixture.trace.creates == 1);
        fixture.Drain(*operation);
        REQUIRE(operation->State() == PrefabSpawnState::Committed);
        auto instance = operation->Spawned();
        REQUIRE(instance.HasValue());
        auto root = fixture.scenes.ActiveScene()->Get(instance.Value().Root());
        REQUIRE(root.HasValue());
        CHECK(root.Value().parent == original);
        CHECK(root.Value().localTransform->translation == Math::Vec3{7, 8, 9});
        REQUIRE(root.Value().components->behaviors.size() == 1);
        CHECK(std::get<double>(root.Value().components->behaviors.front().fields.front().value) == 42.0);
        CHECK(std::get<double>(root.Value().components->behaviors.front().fields[1].value) == 2.0);
        CHECK(root.Value().components->behaviors.front().instanceId.value != 1);
        CHECK(fixture.Count() == 3);
        CHECK(fixture.trace.creates == 3);
        CHECK(fixture.runner->InstanceCount() == 3);
        REQUIRE(fixture.runner->FixedUpdate({}, {0.01}).HasValue());
        CHECK(fixture.trace.starts == 3);
        auto retired = fixture.context->Despawn(instance.Value(), fixture.tick);
        REQUIRE(retired.HasValue());
        fixture.Drain(retired.Value());
        CHECK(retired.Value().State() == PrefabSpawnState::Committed);
        CHECK(fixture.Count() == 1);
        CHECK(fixture.runner->InstanceCount() == 1);
        CHECK(fixture.trace.destroys == 2);
        CHECK(fixture.scenes.ActiveScene()->Get(instance.Value().Root()).HasError());
    }

    TEST_CASE("Spawned prefab member lookup binds exact authored addresses to Scene-owned generations", "[prefab][spawn][provenance]") {
        Fixture fixture;
        auto data = fixture.Data();
        const auto first = PrefabObjectAddress::Create(std::array{LocalObjectId{9}}, {7}).Value();
        const auto second = PrefabObjectAddress::Create(std::array{LocalObjectId{4}}, {7}).Value();
        data.entities[1].provenance.sourceObject = first;
        data.entities[1].localTransform.translation = {10, 0, 0};
        auto repeated = data.entities[1];
        repeated.provenance.sourceObject = second;
        repeated.localTransform.translation = {20, 0, 0};
        data.entities.push_back(std::move(repeated));
        const auto expectedRoot = data.entities.front().provenance;
        const auto expectedFirst = data.entities[1].provenance;
        const auto expectedSecond = data.entities[2].provenance;

        const auto spawn = [&](Fixture &host) {
            host.Put(data);
            auto pending = host.context->Spawn(host.Request());
            REQUIRE(pending.HasValue());
            REQUIRE(pending.Value().Spawned().HasError());
            host.Drain(pending.Value());
            auto result = pending.Value().Spawned();
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        };
        const auto instance = spawn(fixture);
        const auto scene = *fixture.scenes.ActiveScene();
        const auto root = instance.ResolveMember(expectedRoot.sourceObject, scene);
        const auto firstMember = instance.ResolveMember(first, scene);
        const auto secondMember = instance.ResolveMember(second, scene);
        REQUIRE(root.HasValue());
        REQUIRE(firstMember.HasValue());
        REQUIRE(secondMember.HasValue());
        CHECK(root.Value().entity == instance.Root());
        CHECK(root.Value().provenance == expectedRoot);
        CHECK(firstMember.Value().provenance == expectedFirst);
        CHECK(secondMember.Value().provenance == expectedSecond);
        CHECK(firstMember.Value().entity != secondMember.Value().entity);
        CHECK(scene.Get(firstMember.Value().entity).Value().localTransform->translation == Math::Vec3{10, 0, 0});
        CHECK(scene.Get(secondMember.Value().entity).Value().localTransform->translation == Math::Vec3{20, 0, 0});
        const auto absent = instance.ResolveMember(PrefabObjectAddress::Create({}, {7}).Value(), scene);
        REQUIRE(absent.HasError());
        CHECK(absent.ErrorValue().code.Value() == PrefabErrors::AddressInvalid.code.Value());

        SECTION("independent Scene owners can have identical numeric entity coordinates") {
            Fixture other;
            const auto otherInstance = spawn(other);
            const auto otherScene = *other.scenes.ActiveScene();
            REQUIRE(otherInstance.Root() == instance.Root());
            REQUIRE(otherScene.RuntimeId() == scene.RuntimeId());
            CHECK_FALSE(otherScene.SameIncarnation(scene));
            CHECK(instance.ResolveMember(first, otherScene).HasError());
            CHECK(otherInstance.ResolveMember(first, scene).HasError());
        }

        SECTION("an individually retired member cannot resolve through an otherwise live root") {
            Runtime::SceneCommandBuffer retire;
            retire.Destroy(firstMember.Value().entity);
            REQUIRE(fixture.scenes.QueueStructuralCommands(std::move(retire)).HasValue());
            fixture.Commit();
            const auto current = *fixture.scenes.ActiveScene();
            CHECK(current.SameIncarnation(scene));
            CHECK_FALSE(scene.IsCurrent());
            CHECK(instance.ResolveMember(second, scene).HasError());
            CHECK(instance.ResolveMember(expectedRoot.sourceObject, current).HasValue());
            CHECK(instance.ResolveMember(second, current).HasValue());
            CHECK(instance.ResolveMember(first, current).HasError());
        }

        SECTION("Scene replacement retires every mapping") {
            Runtime::SceneDefinitionBuilder builder{{8}, {1}};
            REQUIRE(fixture.scenes.QueuePreparation(std::move(builder).Build().Value()).HasValue());
            fixture.Commit();
            const auto replacement = *fixture.scenes.ActiveScene();
            REQUIRE(replacement.RuntimeId() != scene.RuntimeId());
            CHECK(instance.ResolveMember(first, replacement).HasError());
            CHECK(instance.ResolveMember(first, scene).HasError());
        }

        SECTION("duplicate full addresses are malformed cooked data") {
            data.entities[2].provenance.sourceObject = first;
            const auto malformed = CookedPrefab::Create(std::move(data), PrefabLimitProfile::Create({}).Value());
            REQUIRE(malformed.HasError());
            CHECK(malformed.ErrorValue().code.Value() == PrefabErrors::CookArtifactInvalid.code.Value());
            CHECK(instance.ResolveMember(first, scene).HasValue());
        }
    }

    TEST_CASE("Despawn rejects retained prefab identities after entity generation reuse", "[prefab][spawn][lifecycle]") {
        Fixture fixture;
        auto first = fixture.context->Spawn(fixture.Request());
        REQUIRE(first.HasValue());
        fixture.Drain(first.Value());
        const auto original = first.Value().Spawned().Value();
        auto retired = fixture.context->Despawn(original, fixture.tick);
        REQUIRE(retired.HasValue());
        fixture.Drain(retired.Value());
        REQUIRE(retired.Value().State() == PrefabSpawnState::Committed);
        auto second = fixture.context->Spawn(fixture.Request());
        REQUIRE(second.HasValue());
        fixture.Drain(second.Value());
        REQUIRE(second.Value().State() == PrefabSpawnState::Committed);
        const auto replacement = second.Value().Spawned().Value();
        const auto scene = *fixture.scenes.ActiveScene();
        CHECK(original.ResolveMember({}, scene).HasError());
        CHECK(replacement.ResolveMember({}, scene).HasValue());
        auto repeated = fixture.context->Despawn(original, fixture.tick);
        REQUIRE(repeated.HasValue());
        fixture.Drain(repeated.Value());
        CHECK(repeated.Value().State() == PrefabSpawnState::Failed);
        CHECK(fixture.Count() == 3);
    }

    TEST_CASE("Prefab initialization coverage and constraints fail before structural mutation", "[prefab][spawn][malformed]") {
        Fixture fixture;
        auto request = fixture.Request();
        SECTION("missing required") {
            request.initialization.clear();
        }
        SECTION("undeclared identity") {
            request.initialization.front().id = {8};
        }
        SECTION("wrong type") {
            request.initialization.front().value = std::int64_t{42};
        }
        SECTION("below minimum") {
            request.initialization.front().value = -1.0;
        }
        SECTION("above maximum") {
            request.initialization.front().value = 101.0;
        }
        SECTION("nonfinite") {
            request.initialization.front().value = std::numeric_limits<double>::infinity();
        }
        auto operation = fixture.context->Spawn(request);
        REQUIRE(operation.HasValue());
        fixture.Drain(operation.Value());
        CHECK(operation.Value().State() == PrefabSpawnState::Failed);
        CHECK(operation.Value().Failure() != nullptr);
        CHECK(fixture.Count() == 1);
        CHECK(fixture.trace.creates == 1);
    }

    TEST_CASE("Prefab bounded request admission rejects malformed identities and scheduling", "[prefab][spawn][bounds]") {
        Fixture fixture;
        auto request = fixture.Request();
        SECTION("duplicate initialization") {
            request.initialization.push_back(request.initialization.front());
        }
        SECTION("65 supplied values") {
            request.initialization.resize(65);
        }
        SECTION("unbounded string") {
            request.initialization.front().value = std::string(257, 'x');
        }
        SECTION("zero initialization identity") {
            request.initialization.front().id.value = 0;
        }
        SECTION("null value") {
            request.initialization.front().value = std::monostate{};
        }
        SECTION("invalid parent") {
            request.parent = Runtime::EntityRef{{999}, {0, 1}};
        }
        SECTION("invalid placement") {
            request.placement.translation.x = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("zero tick") {
            request.notBeforeTick = 0;
        }
        SECTION("beyond future window") {
            request.notBeforeTick = 65;
        }
        SECTION("tick overflow") {
            request.notBeforeTick = std::numeric_limits<std::uint64_t>::max();
        }
        auto operation = fixture.context->Spawn(request);
        CHECK(operation.HasError());
        CHECK(fixture.Count() == 1);
    }

    TEST_CASE("Prefab tick addressing delays publication and enforces request capacity", "[prefab][spawn][bounds]") {
        Fixture fixture;
        auto request = fixture.Request();
        request.notBeforeTick = 64;
        auto first = fixture.context->Spawn(request);
        REQUIRE(first.HasValue());
        for (std::size_t index = 1; index < 32; ++index)
            REQUIRE(fixture.context->Spawn(request).HasValue());
        CHECK(fixture.context->Spawn(request).HasError());
        fixture.Advance();
        fixture.Commit();
        CHECK(fixture.Count() == 1);
        CHECK(first.Value().State() == PrefabSpawnState::Pending);
        fixture.context->Revoke();
        fixture.Advance();
        CHECK(first.Value().State() == PrefabSpawnState::Cancelled);
        CHECK(fixture.context->Spawn(request).HasError());
    }

    TEST_CASE("Exactly 64 declared initialization interfaces reach actual behavior construction", "[prefab][spawn][bounds]") {
        Fixture fixture{64};
        auto data = fixture.Data();
        data.initialization.clear();
        auto request = fixture.Request();
        request.initialization.clear();
        for (std::uint32_t index = 0; index < 64; ++index) {
            data.initialization.push_back({{index + 1U}, {{0}, 0}, index, PrefabInitializationKind::Number, true, 0.0, 100.0});
            request.initialization.push_back({{index + 1U}, static_cast<double>(index)});
        }
        fixture.Put(std::move(data));
        request.source.artifactDigest = fixture.digest;
        auto operation = fixture.context->Spawn(request);
        REQUIRE(operation.HasValue());
        fixture.Drain(operation.Value());
        REQUIRE(operation.Value().State() == PrefabSpawnState::Committed);
        const auto root = fixture.scenes.ActiveScene()->Get(operation.Value().Spawned().Value().Root());
        REQUIRE(root.HasValue());
        const auto &fields = root.Value().components->behaviors.front().fields;
        REQUIRE(fields.size() == 64);
        CHECK(std::get<double>(fields.back().value) == 63.0);
    }

    TEST_CASE("Cooked initialization interfaces validate portable identity targets bounds and version",
              "[prefab][cooked][initialization]") {
        Fixture fixture;
        auto data = fixture.Data();
        SECTION("duplicate identity") {
            data.initialization.push_back(data.initialization.front());
        }
        SECTION("target is missing") {
            data.initialization.front().owner.member = 10;
        }
        SECTION("field is missing") {
            data.initialization.front().field = 10;
        }
        SECTION("wrong declared kind") {
            data.initialization.front().kind = PrefabInitializationKind::Integer;
        }
        SECTION("unknown kind") {
            data.initialization.front().kind = static_cast<PrefabInitializationKind>(255);
        }
        SECTION("inverted bounds") {
            data.initialization.front().minimum = 200;
        }
        SECTION("invalid default") {
            data.initialization.front().minimum = 5;
        }
        SECTION("aliasing target") {
            auto alias = data.initialization.front();
            alias.id = {3};
            data.initialization.push_back(alias);
        }
        const auto rejected = CookedPrefab::Create(std::move(data), PrefabLimitProfile::Create({}).Value());
        CHECK(rejected.HasError());
    }

    TEST_CASE("Cooked v2 initialization roundtrips and rejects v1 without reinterpretation", "[prefab][cooked][initialization]") {
        Fixture fixture;
        auto candidate = fixture.Data();
        auto cooked = CookedPrefab::Create(candidate, PrefabLimitProfile::Create({}).Value());
        REQUIRE(cooked.HasValue());
        auto decoded = CookedPrefab::Parse(cooked.Value().Bytes(), candidate.assetId, PrefabLimitProfile::Create({}).Value());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().Data() == candidate);
        std::vector<std::byte> legacy(cooked.Value().Bytes().begin(), cooked.Value().Bytes().end());
        legacy[7] = std::byte{1};
        auto rejected = CookedPrefab::Parse(legacy, candidate.assetId, PrefabLimitProfile::Create({}).Value());
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::UnsupportedCookedVersion.code.Value());
    }

    TEST_CASE("Initialization closed value kinds enforce string finite and quaternion boundaries", "[prefab][initialization][bounds]") {
        using enum PrefabInitializationKind;
        CookedPrefabInitialization declaration;
        declaration.id = {1};
        declaration.kind = String;
        CHECK(ValidatePrefabInitializationValue(declaration, std::string(256, 'x')).HasValue());
        CHECK(ValidatePrefabInitializationValue(declaration, std::string(257, 'x')).HasError());
        declaration.kind = Integer;
        declaration.minimum = -5;
        declaration.maximum = 5;
        CHECK(ValidatePrefabInitializationValue(declaration, std::int64_t{-5}).HasValue());
        CHECK(ValidatePrefabInitializationValue(declaration, std::int64_t{5}).HasValue());
        CHECK(ValidatePrefabInitializationValue(declaration, std::numeric_limits<std::int64_t>::max()).HasError());
        declaration.kind = Vec3;
        CHECK(ValidatePrefabInitializationValue(declaration, Math::Vec3{}).HasValue());
        CHECK(ValidatePrefabInitializationValue(declaration, Math::Vec3{std::numeric_limits<float>::quiet_NaN(), 0, 0}).HasError());
        declaration.kind = Quaternion;
        CHECK(ValidatePrefabInitializationValue(declaration, Math::Quaternion{}).HasValue());
        CHECK(ValidatePrefabInitializationValue(declaration, Math::Quaternion{0, 0, 0, 0}).HasError());
    }
}  // namespace Horo::Prefab
