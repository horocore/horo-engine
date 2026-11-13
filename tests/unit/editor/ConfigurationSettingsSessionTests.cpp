#include "Horo/Editor/EditorConfiguration.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    ConfigurationSchema Schema() {
        ConfigurationSchema schema;
        SettingDescriptor descriptor{.key = SettingKey{"module.limit"},
                                     .type = SettingValueType::Integer,
                                     .defaultValue = std::int64_t{2},
                                     .scope = SettingScope::User,
                                     .reloadPolicy = ReloadPolicy::NextOperation,
                                     .sensitivity = SettingSensitivity::Public};
        REQUIRE(schema.Register(descriptor).HasValue());
        descriptor.key = SettingKey{"module.readonly"};
        descriptor.readOnly = true;
        REQUIRE(schema.Register(descriptor).HasValue());
        descriptor.key = SettingKey{"module.invocation"};
        descriptor.readOnly = false;
        descriptor.scope = SettingScope::Invocation;
        REQUIRE(schema.Register(descriptor).HasValue());
        descriptor.key = SettingKey{"module.credential"};
        descriptor.type = SettingValueType::CredentialReference;
        descriptor.defaultValue = CredentialReference{};
        descriptor.scope = SettingScope::User;
        descriptor.sensitivity = SettingSensitivity::SecretReference;
        REQUIRE(schema.Register(descriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        return schema;
    }

    TEST_CASE("Descriptor settings sessions validate preview cancel and conflicts", "[unit][editor][configuration]") {
        ConfigurationService configuration{Schema()};
        ConfigurationSettingsSession session{configuration};
        REQUIRE(session.Fields().size() == 4);
        CHECK_FALSE(session.Fields()[0].editable);
        CHECK(session.Fields()[0].text.empty());
        REQUIRE(session.Edit(SettingKey{"module.credential"}, "secret-text").HasError());
        REQUIRE(session.Edit(SettingKey{"module.readonly"}, "8").HasError());
        REQUIRE(session.Edit(SettingKey{"module.invocation"}, "8").HasError());
        const SettingKey key{"module.limit"};
        REQUIRE(session.Edit(key, "8").HasValue());
        REQUIRE(session.Preview());
        CHECK(std::get<std::int64_t>(session.Preview()->Get(key)) == 8);
        CHECK(std::get<std::int64_t>(configuration.Snapshot().Get(key)) == 2);
        session.Cancel();
        CHECK_FALSE(session.Preview());
        CHECK(session.Draft().proposedValues.empty());
        CHECK(configuration.Snapshot().Revision() == 0);
        REQUIRE(session.Edit(key, "invalid-number").HasError());
        REQUIRE(session.Apply().HasError());
        CHECK(configuration.Snapshot().Revision() == 0);
        REQUIRE(session.Edit(key, "8").HasValue());
        ConfigurationDraft external{.baseRevision = 0, .source = ConfigurationSource::User};
        external.proposedValues.emplace(key, std::int64_t{6});
        REQUIRE(configuration.Commit(external).HasValue());
        auto stale = session.Apply();
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == "configuration.draft_stale");
        CHECK(session.Draft().baseRevision == configuration.Snapshot().Revision());
        CHECK(session.Draft().proposedValues.empty());
        CHECK(std::get<std::int64_t>(configuration.Snapshot().Get(key)) == 6);
        REQUIRE(session.Edit(key, "9").HasValue());
        REQUIRE(session.Apply().HasValue());
        CHECK(std::get<std::int64_t>(configuration.Snapshot().Get(key)) == 9);
        REQUIRE(session.RestoreDefaults().HasValue());
        REQUIRE(session.Preview());
        CHECK(std::get<std::int64_t>(session.Preview()->Get(key)) == 2);
        CHECK(std::get<std::int64_t>(configuration.Snapshot().Get(key)) == 9);
        session.Cancel();
        CHECK(std::get<std::int64_t>(configuration.Snapshot().Get(key)) == 9);
    }
}  // namespace
