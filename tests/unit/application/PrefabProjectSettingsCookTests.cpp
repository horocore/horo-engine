#include "HeadlessProjectScene.h"
#include "Horo/Application/PrefabProjectSettings.h"
#include "PrefabSceneCookHostFixture.h"

using namespace Horo;
using namespace Horo::Application::PrefabCookTestSupport;

TEST_CASE("Prefab scene cook consumes persisted project limits and invalidates canonical cache identity", "[prefab][settings][cook]") {
    HostFixture fixture;
    const auto original = fixture.Cook();
    REQUIRE(original.HasValue());
    auto values = Application::PrefabPolicyFieldValues({});
    values[static_cast<std::size_t>(Application::PrefabLimitField::ObjectCount)] = 2;
    const auto settings = Application::PrefabProjectSettings::Create(values);
    REQUIRE(settings.HasValue());
    fixture.WriteText(".horo/prefab.json", settings.Value().Configuration().ToJson());
    const auto bounded = fixture.Cook();
    REQUIRE(bounded.HasValue());
    CHECK(bounded.Value().generation.manifestDigest != original.Value().generation.manifestDigest);
    values[static_cast<std::size_t>(Application::PrefabLimitField::ObjectCount)] = 1;
    fixture.WriteText(".horo/prefab.json", Application::PrefabProjectSettings::Create(values).Value().Configuration().ToJson());
    const auto over = fixture.Cook();
    CHECK(over.HasError());
    fixture.AssertRetained(bounded.Value().generation);
    fixture.WriteText(".horo/prefab.json", "{broken");
    CHECK(fixture.Cook().HasError());
    fixture.AssertRetained(bounded.Value().generation);
}

TEST_CASE("Project CLI resources capture accepted prefab policy without retargeting an existing host capture", "[prefab][settings][host]") {
    HostFixture fixture;
    std::filesystem::rename(fixture.project.dir.path / "assets/level.scene", fixture.project.dir.path / "assets/level.horo");
    std::filesystem::rename(fixture.project.dir.path / "assets/level.scene.horo", fixture.project.dir.path / "assets/level.horo.horo");
    const auto registry = fixture.registry.Snapshot();
    std::vector<Assets::AssetRecord> records{registry.Records().begin(), registry.Records().end()};
    for (auto &record : records) {
        if (record.id == fixture.sceneId) {
            record.sourcePath = ProjectPath::Parse("assets/level.horo").Value();
            record.metadataPath = ProjectPath::Parse("assets/level.horo.horo").Value();
        }
    }
    REQUIRE(fixture.registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
    auto values = Application::PrefabPolicyFieldValues({});
    values[static_cast<std::size_t>(Application::PrefabLimitField::RuntimeSpawnDepth)] = 2;
    values[static_cast<std::size_t>(Application::PrefabLimitField::InstanceBindings)] = 1;
    const auto policy = Application::PrefabProjectSettings::Create(values);
    REQUIRE(policy.HasValue());
    fixture.WriteText(".horo/prefab.json", policy.Value().Configuration().ToJson());
    REQUIRE(fixture.Cook().HasValue());
    Application::Internal::HeadlessProjectSceneOptions input{.projectRoot = fixture.project.dir.path,
                                                             .scene = "cooked:" + fixture.sceneId.ToString(),
                                                             .cookedRoot = fixture.cooked.path,
                                                             .cookTarget = fixture.request.assets.target.Value(),
                                                             .physicsMode = Physics::PhysicsRuntimeMode::Null};
    auto first = Application::Internal::PrepareHeadlessProjectSceneResources(input);
    if (first.HasError())
        UNSCOPED_INFO(first.ErrorValue().code.Value() << ": " << first.ErrorValue().message);
    REQUIRE(first.HasValue());
    CHECK(first.Value()->prefabPolicy == policy.Value().Limits().Policy());
    values[static_cast<std::size_t>(Application::PrefabLimitField::RuntimeSpawnDepth)] = 1;
    const auto replacement = Application::PrefabProjectSettings::Create(values);
    REQUIRE(replacement.HasValue());
    fixture.WriteText(".horo/prefab.json", replacement.Value().Configuration().ToJson());
    auto second = Application::Internal::PrepareHeadlessProjectSceneResources(input);
    REQUIRE(second.HasValue());
    CHECK(second.Value()->prefabPolicy == replacement.Value().Limits().Policy());
    CHECK(first.Value()->prefabPolicy.maximumRuntimeSpawnDepth == 2);
    fixture.WriteText(".horo/prefab.json", "{invalid");
    CHECK(Application::Internal::PrepareHeadlessProjectSceneResources(input).HasError());
    CHECK(first.Value()->prefabPolicy.maximumRuntimeSpawnDepth == 2);
}
