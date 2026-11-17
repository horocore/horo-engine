#include "PrefabSettingsTestSupport.h"

#include <thread>
#include <type_traits>

using namespace Horo;
using namespace Horo::Tests::PrefabSettingsSupport;
using Field = Application::PrefabLimitField;
using Command = Editor::PrefabSettingsCommandKind;

TEST_CASE("Prefab project settings registry exposes every canonical configurable field and derived work", "[prefab][settings][policy]") {
    const auto defaults = Application::PrefabProjectSettings::Resolve({});
    REQUIRE(defaults.HasValue());
    CHECK(defaults.Value().Limits().Policy() == Prefab::PrefabProjectPolicy{});
    CHECK(Application::PrefabLimitFields().size() == 19);
    const auto values = Application::PrefabPolicyFieldValues(defaults.Value().Limits().Policy());
    for (const auto &field : Application::PrefabLimitFields()) {
        auto invalid = values;
        const auto index = static_cast<std::size_t>(field.field);
        CHECK(values[index] == static_cast<std::int64_t>(field.defaultValue));
        invalid[index] = static_cast<std::int64_t>(field.hardCeiling) + 1;
        const auto result = Application::ValidatePrefabPolicyValues(invalid);
        CHECK_FALSE(result.profile);
        CHECK(result.fields[index] != Application::PrefabPolicyFinding::None);
        invalid[index] = 0;
        CHECK_FALSE(Application::ValidatePrefabPolicyValues(invalid).profile);
        invalid[index] = -1;
        CHECK_FALSE(Application::ValidatePrefabPolicyValues(invalid).profile);
    }
    auto changed = values;
    changed[static_cast<std::size_t>(Field::ObjectCount)] = 10;
    const auto accepted = Application::PrefabProjectSettings::Create(changed);
    REQUIRE(accepted.HasValue());
    CHECK(accepted.Value().Limits().MaximumExpansionWorkItems() < defaults.Value().Limits().MaximumExpansionWorkItems());
    changed[static_cast<std::size_t>(Field::OverrideSetBytes)] = 1;
    const auto inconsistent = Application::ValidatePrefabPolicyValues(changed);
    CHECK_FALSE(inconsistent.profile);
    CHECK(inconsistent.fields[static_cast<std::size_t>(Field::OverrideSetBytes)] == Application::PrefabPolicyFinding::ValueExceedsSet);
}

TEST_CASE("Prefab configuration uses canonical strict typed resolver and deterministic persistence", "[prefab][settings][configuration]") {
    const auto schema = Application::CreatePrefabProjectSettingsSchema();
    REQUIRE(schema.HasValue());
    const auto defaults = Application::PrefabProjectSettings::Resolve({}).Value();
    const auto bytes = defaults.Configuration().ToJson();
    const auto parsed = ConfigurationResolver::ParseDocument(schema.Value(), bytes, "prefab.json");
    REQUIRE(parsed.HasValue());
    const auto decoded = Application::PrefabProjectSettings::Resolve(parsed.Value());
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value().Configuration().ToJson() == bytes);
    for (const auto source : {R"({"schemaVersion":1,"values":{"prefab.limits.object_count":1.5}})",
                              R"({"schemaVersion":1,"values":{"prefab.limits.object_count":"3"}})", R"({"schemaVersion":2,"values":{}})",
                              R"({"schemaVersion":1,"values":{"prefab.limits.object_count":2,"prefab.limits.object_count":3}})",
                              R"({"schemaVersion":1,"values":{},"extra":0})"}) {
        const auto parsedInput = ConfigurationResolver::ParseDocument(schema.Value(), source, "prefab.json");
        CHECK((parsedInput.HasError() || Application::PrefabProjectSettings::Resolve(parsedInput.Value()).HasError()));
    }
    const auto unknown =
        ConfigurationResolver::ParseDocument(schema.Value(), R"({"schemaVersion":1,"values":{"prefab.unknown":1}})", "prefab.json");
    if (unknown.HasValue())
        CHECK(Application::PrefabProjectSettings::Resolve(unknown.Value()).HasError());
    ConfigurationResolutionRequest forbidden;
    forbidden.user.emplace(SettingKey{"prefab.limits.object_count"}, ConfigurationInputValue{std::int64_t{2}, {}});
    CHECK(ConfigurationResolver::Resolve(schema.Value(), forbidden).HasError());
}

TEST_CASE("Prefab settings owner edits reset discards and durably publishes exact immutable next-operation policy",
          "[prefab][settings][session]") {
    Fixture fixture;
    const auto old = fixture.owner->Accepted().Value();
    CHECK_FALSE(fixture.owner->IsDirty());
    CHECK_FALSE(std::filesystem::exists(fixture.root / ".horo/prefab.json"));
    REQUIRE(fixture.Command(Command::Edit, Field::ObjectCount, 10).HasValue());
    CHECK(fixture.owner->IsDirty());
    CHECK(fixture.owner->Accepted().Value() == old);
    const auto stale = fixture.owner->Snapshot().Value();
    REQUIRE(fixture.Command(Command::ResetDefaults).HasValue());
    CHECK_FALSE(fixture.owner->IsDirty());
    CHECK(fixture.owner->Execute({Command::Save, stale->revision}).HasError());
    REQUIRE(fixture.Command(Command::Edit, Field::ObjectCount, 12).HasValue());
    REQUIRE(fixture.Command(Command::Save).HasValue());
    CHECK_FALSE(fixture.owner->IsDirty());
    CHECK(fixture.files.replacements == 1);
    CHECK(old->Limits().Policy().maximumObjectCount == 256);
    CHECK(fixture.owner->Accepted().Value()->Limits().Policy().maximumObjectCount == 12);
    const auto loaded = Application::LoadPrefabProjectSettings(fixture.root);
    REQUIRE(loaded.HasValue());
    CHECK(loaded.Value().settings->Limits().Policy().maximumObjectCount == 12);
    REQUIRE(fixture.Command(Command::ResetDefaults).HasValue());
    CHECK(fixture.owner->IsDirty());
    REQUIRE(fixture.Command(Command::Discard).HasValue());
    CHECK_FALSE(fixture.owner->IsDirty());
    CHECK(fixture.owner->Snapshot().Value()->values[static_cast<std::size_t>(Field::ObjectCount)] == 12);
}

TEST_CASE("Prefab settings rejects invalid conflicting and busy saves before source replacement", "[prefab][settings][failure]") {
    Fixture fixture;
    REQUIRE(fixture.Command(Command::Edit, Field::ObjectCount, 0).HasValue());
    CHECK(fixture.Command(Command::Save).HasError());
    CHECK(fixture.files.replacements == 0);
    REQUIRE(fixture.Command(Command::InvalidNumber, Field::ObjectCount).HasValue());
    CHECK(fixture.owner->Snapshot().Value()->validation.fields[static_cast<std::size_t>(Field::ObjectCount)] ==
          Application::PrefabPolicyFinding::InvalidNumber);
    CHECK(fixture.Command(Command::Save).HasError());
    REQUIRE(fixture.Command(Command::Edit, Field::ObjectCount, 10).HasValue());
    {
        const auto lease = fixture.mutations.TryAcquire({fixture.root, Editor::ProjectMutationOwner::Asset, "test.busy"});
        REQUIRE(lease.HasValue());
        CHECK(fixture.Command(Command::Save).HasError());
    }
    CHECK(fixture.files.replacements == 0);
    fixture.Write(Application::PrefabProjectSettings::Resolve({}).Value().Configuration().ToJson());
    CHECK(fixture.Command(Command::Save).HasError());
    CHECK(fixture.owner->IsDirty());
    CHECK(fixture.files.replacements == 0);
    CHECK(fixture.owner->Accepted().Value()->Limits().Policy().maximumObjectCount == 256);
}

TEST_CASE("Prefab settings failed reload preserves exact accepted publication and dirty draft", "[prefab][settings][reload]") {
    Fixture fixture;
    const auto accepted = fixture.owner->Accepted().Value();
    fixture.Write("{malformed");
    const auto before = fixture.owner->Snapshot().Value();
    CHECK(fixture.Command(Command::Reload).HasError());
    CHECK(fixture.owner->Snapshot().Value() == before);
    CHECK(fixture.owner->Accepted().Value() == accepted);
    REQUIRE(fixture.Command(Command::Edit, Field::ObjectCount, 12).HasValue());
    const auto dirty = fixture.owner->Snapshot().Value();
    CHECK(fixture.Command(Command::Reload).HasError());
    CHECK(fixture.owner->Snapshot().Value() == dirty);
    REQUIRE(fixture.Command(Command::Discard).HasValue());
    fixture.Write(accepted->Configuration().ToJson());
    REQUIRE(fixture.Command(Command::Reload).HasValue());
    CHECK_FALSE(fixture.owner->IsDirty());
}

TEST_CASE("Prefab settings native publication failure and uncertainty retain truthful owner state", "[prefab][settings][durability]") {
    Fixture fixture;
    REQUIRE(fixture.Command(Command::Edit, Field::ObjectCount, 12).HasValue());
    const auto accepted = fixture.owner->Accepted().Value();
    fixture.files.fault = Files::Fault::LockException;
    const auto lockFailure = fixture.Command(Command::Save);
    REQUIRE(lockFailure.HasError());
    CHECK(lockFailure.ErrorValue().code.Value() == Assets::AssetSourceErrors::Authority.code.Value());
    CHECK(fixture.owner->Accepted().Value() == accepted);
    CHECK(fixture.files.replacements == 0);
    fixture.files.fault = Files::Fault::BeforeReplace;
    CHECK(fixture.Command(Command::Save).HasError());
    CHECK(fixture.owner->Accepted().Value() == accepted);
    CHECK(fixture.owner->IsDirty());
    fixture.files.fault = Files::Fault::AfterReplace;
    REQUIRE(fixture.Command(Command::Save).HasValue());
    CHECK(fixture.owner->IsDirty());
    REQUIRE(fixture.owner->Snapshot().Value()->durabilityError);
    CHECK(fixture.owner->Accepted().Value()->Limits().Policy().maximumObjectCount == 12);
    REQUIRE(fixture.Command(Command::Discard).HasValue());
    CHECK(fixture.owner->IsDirty());
    REQUIRE(fixture.owner->Snapshot().Value()->durabilityError);
    fixture.files.fault = Files::Fault::None;
    REQUIRE(fixture.Command(Command::Save).HasValue());
    CHECK_FALSE(fixture.owner->IsDirty());
}

TEST_CASE("Prefab settings closes exact openings and rejects foreign thread commands", "[prefab][settings][lifecycle]") {
    Fixture fixture;
    bool rejected{};
    std::thread foreign([&] {
        rejected = fixture.owner->Snapshot().HasError();
    });
    foreign.join();
    CHECK(rejected);
    const auto retained = fixture.owner->Snapshot().Value();
    const auto accepted = fixture.owner->Accepted().Value();
    fixture.owner->Close();
    CHECK(fixture.owner->Snapshot().HasError());
    CHECK(fixture.owner->Execute({Command::ResetDefaults, retained->revision}).HasError());
    CHECK(retained->validation.profile.has_value());
    CHECK(accepted->Limits().Policy() == Prefab::PrefabProjectPolicy{});
}

TEST_CASE("Prefab project source capture rejects oversized and unknown documents without replacing an accepted snapshot",
          "[prefab][settings][bounded-source]") {
    Fixture fixture;
    const auto before = fixture.owner->Snapshot().Value();
    for (const auto &bytes :
         {std::string(64U * 1024U + 1U, ' '), std::string{R"({"schemaVersion":1,"values":{"prefab.limits.unknown":2}})"}}) {
        fixture.Write(bytes);
        CHECK(fixture.Command(Command::Reload).HasError());
        CHECK(fixture.owner->Snapshot().Value() == before);
    }
}
