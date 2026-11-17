#include "Horo/Debug/ConsoleVariable.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace Horo;
using namespace Horo::Debug;

TEST_CASE("Console session variables validate every type and explicit activation policy", "[console]") {
    const std::array<ConsoleValue, 4> defaults{false, std::int64_t{2}, 2.0, std::string("old")};
    const std::array<ConsoleValue, 4> replacements{true, std::int64_t{4}, 4.0, std::string("new")};
    for (std::size_t i = 0; i < defaults.size(); ++i) {
        for (auto policy :
             {ConsoleVariableApplyPolicy::Immediate, ConsoleVariableApplyPolicy::NextFrame, ConsoleVariableApplyPolicy::NextSceneLoad,
              ConsoleVariableApplyPolicy::RestartRequired, ConsoleVariableApplyPolicy::ReadOnly}) {
            ConsoleVariableDescriptor descriptor{1, "console.value", defaults[i]};
            descriptor.applyPolicy = policy;
            auto created = ConsoleVariable::Create(descriptor, {});
            REQUIRE(created.HasValue());
            auto variable = std::move(created).Value();
            CHECK(variable.Read({}).HasError());
            auto changed = variable.Set(replacements[i], {3});
            CHECK(changed.HasValue() == (policy != ConsoleVariableApplyPolicy::ReadOnly));
            auto snapshot = variable.Read({3});
            REQUIRE(snapshot.HasValue());
            if (policy == ConsoleVariableApplyPolicy::Immediate)
                CHECK(snapshot.Value().active == replacements[i]);
            else
                CHECK(snapshot.Value().active == defaults[i]);
            if (policy != ConsoleVariableApplyPolicy::Immediate && policy != ConsoleVariableApplyPolicy::ReadOnly) {
                REQUIRE(snapshot.Value().pending);
                CHECK(*snapshot.Value().pending == replacements[i]);
                auto unrelated = variable.Apply(ConsoleVariableApplyPolicy::Immediate);
                REQUIRE(unrelated.HasValue());
                CHECK_FALSE(unrelated.Value());
                auto applied = variable.Apply(policy);
                REQUIRE(applied.HasValue());
                CHECK(applied.Value());
                auto active = variable.Read({3});
                REQUIRE(active.HasValue());
                CHECK(active.Value().active == replacements[i]);
                CHECK_FALSE(active.Value().pending);
            }
        }
    }
}

TEST_CASE("Console variable ranges exact types and string budgets reject before mutation", "[console]") {
    ConsoleVariableDescriptor descriptor{1, "renderer.debug.limit", std::int64_t{2}, 1, 4};
    auto created = ConsoleVariable::Create(descriptor, {});
    REQUIRE(created.HasValue());
    auto variable = std::move(created).Value();
    for (auto value : {ConsoleValue{std::int64_t{0}}, ConsoleValue{std::int64_t{5}}, ConsoleValue{2.0}, ConsoleValue{true}})
        CHECK(variable.Set(value, {3}).HasError());
    auto value = variable.Read({3});
    REQUIRE(value.HasValue());
    CHECK(std::get<std::int64_t>(value.Value().active) == 2);
    descriptor.defaultValue = std::numeric_limits<double>::quiet_NaN();
    CHECK(ConsoleVariable::Create(descriptor, {}).HasError());
    ConsoleVariableDescriptor text{2, "console.text", std::string("ok")};
    text.maximumStringBytes = 4;
    auto made = ConsoleVariable::Create(text, {});
    REQUIRE(made.HasValue());
    CHECK(made.Value().Descriptor().name == "console.text");
    auto textVariable = std::move(made).Value();
    CHECK(textVariable.Set(std::string("oversized"), {3}).HasError());
    text.persistence = ConsoleVariablePersistence::Project;
    CHECK(ConsoleVariable::Create(text, {}).HasError());
    RegistryPolicy shipping;
    shipping.profile = ProductProfile::Shipping;
    CHECK(ConsoleVariable::Create(descriptor, shipping).HasError());
}

TEST_CASE("Console configuration variables always read and commit the canonical service snapshot", "[console]") {
    ConfigurationSchema schema;
    const SettingKey key("runtime.debug_limit");
    REQUIRE(schema
                .Register({key,
                           SettingValueType::Integer,
                           std::int64_t{2},
                           SettingScope::Session,
                           ReloadPolicy::NextFrame,
                           SettingSensitivity::Public,
                           {}})
                .HasValue());
    REQUIRE(schema.Seal().HasValue());
    auto configuration = std::make_shared<ConfigurationService>(std::move(schema));
    ConsoleVariableDescriptor descriptor{1, "console.limit", std::int64_t{2}, 1, 10};
    descriptor.configurationKey = key;
    descriptor.applyPolicy = ConsoleVariableApplyPolicy::NextFrame;
    auto created = ConsoleVariable::Create(descriptor, {}, configuration);
    REQUIRE(created.HasValue());
    auto variable = std::move(created).Value();
    REQUIRE(variable.Set(std::int64_t{4}, {3}).HasValue());
    CHECK(std::get<std::int64_t>(configuration->Snapshot().Get(key)) == 2);
    REQUIRE(variable.Apply(ConsoleVariableApplyPolicy::NextFrame).HasValue());
    CHECK(std::get<std::int64_t>(configuration->Snapshot().Get(key)) == 4);
    REQUIRE(configuration->Commit({configuration->Snapshot().Revision(), {{key, std::int64_t{7}}}}).HasValue());
    auto read = variable.Read({3});
    REQUIRE(read.HasValue());
    CHECK(std::get<std::int64_t>(read.Value().active) == 7);
    CHECK_FALSE(read.Value().pending);
    REQUIRE(variable.Set(std::int64_t{4}, {3}).HasValue());
    REQUIRE(configuration->Commit({configuration->Snapshot().Revision(), {{key, std::int64_t{8}}}}).HasValue());
    CHECK(variable.Apply(ConsoleVariableApplyPolicy::NextFrame).HasError());
    CHECK(std::get<std::int64_t>(configuration->Snapshot().Get(key)) == 8);
    REQUIRE(variable.Set(std::int64_t{4}, {3}).HasValue());
    REQUIRE(variable.Apply(ConsoleVariableApplyPolicy::NextFrame).HasValue());
    CHECK(std::get<std::int64_t>(configuration->Snapshot().Get(key)) == 4);
}
