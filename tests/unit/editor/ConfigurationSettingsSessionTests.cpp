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

    /** @brief Declares a Project-only pair whose cross-field relation is owned by the injected domain validator. */
    ConfigurationSchema ProjectSchema() {
        ConfigurationSchema schema;
        for (const auto *key : {"project.first", "project.second", "project.unowned"}) {
            REQUIRE(schema
                        .Register({.key = SettingKey{key},
                                   .type = SettingValueType::Integer,
                                   .defaultValue = std::int64_t{2},
                                   .scope = SettingScope::Project,
                                   .reloadPolicy = ReloadPolicy::ProjectReopen,
                                   .sensitivity = SettingSensitivity::Public,
                                   .sourcePolicy = ConfigurationSourcePolicy{.allowedSources = ConfigurationSourceMask::Project}})
                        .HasValue());
        }
        REQUIRE(schema.Seal().HasValue());
        return schema;
    }

    /** @brief Supplies distinguishable original source failures without depending on private editor descriptors. */
    Error SourceError(const char *code) {
        return {.code = ErrorCode{code}, .domain = ErrorDomainId{"test.project.settings"}, .message = "Original source failure."};
    }

    /** @brief Keeps one typed Project authority and observable source-owner failures across independent transaction scenarios. */
    struct ProjectFixture final {
        ConfigurationService configuration{ProjectSchema()};
        bool available = true;
        bool rejectPersistence = false;
        int writes = 0;
        std::int64_t persisted = 2;
        ConfigurationSettingsSession session{configuration,
                                             {.source = ConfigurationSource::Project,
                                              .keys = {SettingKey{"project.first"}, SettingKey{"project.second"}},
                                              .checkAvailable =
                                                  [this] {
            return available ? Result<void>::Success() : Result<void>::Failure(SourceError("project.closed"));
        },
                                              .validate =
                                                  [](const ConfigurationSnapshot &snapshot) {
            const auto first = std::get<std::int64_t>(snapshot.Get(SettingKey{"project.first"}));
            const auto second = std::get<std::int64_t>(snapshot.Get(SettingKey{"project.second"}));
            return first <= second ? Result<void>::Success() : Result<void>::Failure(SourceError("project.range"));
        },
                                              .persist = [this](const ConfigurationSnapshot &snapshot) {
            ++writes;
            CHECK(configuration.Snapshot().Revision() == session.Draft().baseRevision);
            CHECK(snapshot.FindResolved(SettingKey{"project.first"})->source == ConfigurationSource::Project);
            if (rejectPersistence)
                return Result<void>::Failure(SourceError("project.write"));
            persisted = std::get<std::int64_t>(snapshot.Get(SettingKey{"project.first"}));
            return Result<void>::Success();
        }}};
    };

    TEST_CASE("Shared descriptor sessions use exact project source and one prepared persistence",
              "[unit][editor][configuration][project]") {
        ProjectFixture fixture;
        auto &session = fixture.session;
        ConfigurationSettingsSession user{fixture.configuration};
        REQUIRE(user.Edit(SettingKey{"project.first"}, "3").HasError());
        REQUIRE(session.Draft().source == ConfigurationSource::Project);
        REQUIRE(session.Fields().size() == 2);
        REQUIRE(session.Edit(SettingKey{"project.unowned"}, "7").HasError());
        REQUIRE(session.Edit(SettingKey{"project.first"}, "1").HasValue());
        session.Cancel();
        CHECK(fixture.writes == 0);
        CHECK(fixture.persisted == 2);
        CHECK(fixture.configuration.Snapshot().Revision() == 0);
        // Typed cross-field-invalid drafts can be repaired by editing a second field before Apply.
        const auto invalid = session.Edit(SettingKey{"project.first"}, "7");
        REQUIRE(invalid.HasError());
        CHECK(invalid.ErrorValue().code.Value() == "project.range");
        REQUIRE(session.Apply().HasError());
        CHECK(fixture.writes == 0);
        REQUIRE(session.Edit(SettingKey{"project.second"}, "8").HasValue());
        fixture.rejectPersistence = true;
        const auto failed = session.Apply();
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == "project.write");
        CHECK(fixture.configuration.Snapshot().Revision() == 0);
        CHECK(fixture.persisted == 2);
        REQUIRE(session.Preview());
        fixture.rejectPersistence = false;
        REQUIRE(session.Apply().HasValue());
        CHECK(fixture.writes == 2);
        CHECK(fixture.persisted == 7);
        CHECK(fixture.configuration.Snapshot().Revision() == 1);
        CHECK(session.Draft().proposedValues.empty());
    }

    TEST_CASE("Project source sessions reject stale and revoked writers without another durable attempt",
              "[unit][editor][configuration][project]") {
        ProjectFixture fixture;
        auto &session = fixture.session;
        REQUIRE(session.Edit(SettingKey{"project.first"}, "1").HasValue());
        REQUIRE(session.Apply().HasValue());
        REQUIRE(session.Edit(SettingKey{"project.first"}, "0").HasValue());
        ConfigurationDraft external{.baseRevision = 1, .source = ConfigurationSource::Project};
        external.proposedValues.emplace(SettingKey{"project.first"}, std::int64_t{2});
        REQUIRE(fixture.configuration.Commit(external).HasValue());
        const auto stale = session.Apply();
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == "configuration.draft_stale");
        CHECK(fixture.writes == 1);
        CHECK(session.Draft().baseRevision == 2);
        CHECK(session.Draft().proposedValues.empty());
        REQUIRE(session.Edit(SettingKey{"project.first"}, "1").HasValue());
        fixture.available = false;
        for (auto result : {session.Edit(SettingKey{"project.first"}, "0"), session.Apply(), session.RestoreDefaults()}) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == "project.closed");
        }
        CHECK(fixture.writes == 1);
        CHECK_FALSE(session.Preview());
        CHECK(fixture.configuration.Snapshot().Revision() == 2);
    }

    TEST_CASE("Missing source persistence callbacks cannot fall through to memory commit", "[unit][editor][configuration][project]") {
        ConfigurationService configuration{ProjectSchema()};
        ConfigurationSettingsSession session{configuration, ConfigurationSettingsPersistence{}};
        CHECK(session.Fields().empty());
        REQUIRE(session.Failure());
        REQUIRE(session.Edit(SettingKey{"project.first"}, "1").HasError());
        REQUIRE(session.Apply().HasError());
        REQUIRE(session.RestoreDefaults().HasError());
        CHECK(configuration.Snapshot().Revision() == 0);
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
