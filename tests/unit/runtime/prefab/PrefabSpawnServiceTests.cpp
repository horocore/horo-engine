#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabSpawnTestSupport.h"

#include <limits>

namespace Horo::Prefab {
    using namespace SpawnTest;

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
