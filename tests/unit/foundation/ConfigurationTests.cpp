#include "Horo/Foundation/Configuration.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/Platform.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;

    constexpr ConfigurationSourceMask kEveryExternalSource = ConfigurationSourceMask::Invocation | ConfigurationSourceMask::Environment |
                                                             ConfigurationSourceMask::Session | ConfigurationSourceMask::Project |
                                                             ConfigurationSourceMask::User | ConfigurationSourceMask::PackagedProfile;

    const SettingDescriptor kThemeDescriptor{
        .key = SettingKey{"editor.theme.active"},
        .type = SettingValueType::String,
        .defaultValue = std::string{"midnight"},
        .scope = SettingScope::User,
        .reloadPolicy = ReloadPolicy::NextFrame,
        .sensitivity = SettingSensitivity::Public,
    };

    const SettingDescriptor kAutosaveDescriptor{
        .key = SettingKey{"editor.autosave.enabled"},
        .type = SettingValueType::Boolean,
        .defaultValue = true,
        .scope = SettingScope::User,
        .reloadPolicy = ReloadPolicy::Immediate,
        .sensitivity = SettingSensitivity::Public,
    };

    [[nodiscard]] SettingDescriptor ResolutionDescriptor(const bool sessionOverridesEnvironment = false) {
        return {.key = SettingKey{"runtime.worker_count"},
                .type = SettingValueType::Integer,
                .defaultValue = std::int64_t{1},
                .scope = SettingScope::Engine,
                .reloadPolicy = ReloadPolicy::NextOperation,
                .sensitivity = SettingSensitivity::Public,
                .sourcePolicy = ConfigurationSourcePolicy{.allowedSources = kEveryExternalSource,
                                                          .sessionOverridesEnvironment = sessionOverridesEnvironment}};
    }

    [[nodiscard]] ConfigurationSchema BuildSchema(const bool sessionOverridesEnvironment = false) {
        ConfigurationSchema schema;
        REQUIRE(schema.Register(ResolutionDescriptor(sessionOverridesEnvironment)).HasValue());
        REQUIRE(schema.Seal().HasValue());
        return schema;
    }

    [[nodiscard]] ConfigurationService BuildService() {
        ConfigurationSchema schema;
        REQUIRE(schema.Register(kThemeDescriptor).HasValue());
        REQUIRE(schema.Register(kAutosaveDescriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        return ConfigurationService(std::move(schema));
    }

    [[nodiscard]] ConfigurationInputValue Input(SettingValue value, std::string source) {
        return {.value = std::move(value), .location = SourceLocation{.source = std::move(source)}};
    }

    class FixedProcessService final : public ProcessService {
    public:
        [[nodiscard]] ProcessMetadata CurrentProcess() const override {
            return {.id = 42, .executableName = "configuration-test"};
        }

        [[nodiscard]] std::optional<std::string> EnvironmentValue(const std::string_view name) const override {
            const auto found = values.find(std::string{name});
            return found == values.end() ? std::nullopt : std::optional<std::string>{found->second};
        }

        std::unordered_map<std::string, std::string> values;
    };

    TEST_CASE("Schema Rejects Duplicate And Invalid Descriptors", "[unit][foundation][configuration]") {
        ConfigurationSchema schema;
        REQUIRE(schema.Register(kThemeDescriptor).HasValue());
        REQUIRE(schema.Register(kThemeDescriptor).HasError());
        SettingDescriptor empty = kAutosaveDescriptor;
        empty.key = SettingKey{""};
        REQUIRE(schema.Register(empty).HasError());
        REQUIRE(schema.Seal().HasValue());
        REQUIRE(schema.Register(kAutosaveDescriptor).HasError());
        REQUIRE(schema.Seal().HasError());
    }

    TEST_CASE("Snapshot Starts With Schema Defaults And Retains Captured Revisions", "[unit][foundation][configuration]") {
        ConfigurationService service = BuildService();
        const ConfigurationSnapshot before = service.Snapshot();
        REQUIRE(before.Revision() == 0);
        REQUIRE(std::get<std::string>(before.Get(SettingKey{"editor.theme.active"})) == "midnight");
        REQUIRE(before.FindResolved(SettingKey{"editor.theme.active"})->source == ConfigurationSource::SchemaDefault);

        ConfigurationDraft draft{.baseRevision = before.Revision()};
        draft.proposedValues.try_emplace(SettingKey{"editor.theme.active"}, std::string{"light"});
        draft.proposedValues.try_emplace(SettingKey{"editor.autosave.enabled"}, false);
        REQUIRE(service.Commit(draft).HasValue());

        const ConfigurationSnapshot after = service.Snapshot();
        REQUIRE(after.Revision() == 1);
        REQUIRE(std::get<std::string>(before.Get(SettingKey{"editor.theme.active"})) == "midnight");
        REQUIRE(std::get<std::string>(after.Get(SettingKey{"editor.theme.active"})) == "light");
        REQUIRE(after.FindResolved(SettingKey{"editor.theme.active"})->source == ConfigurationSource::Session);
    }

    TEST_CASE("Resolver Applies Every Canonical Precedence Layer With Provenance", "[unit][foundation][configuration]") {
        const ConfigurationSchema schema = BuildSchema();
        const auto resolve = [&schema](ConfigurationResolutionRequest request) {
            Result<ConfigurationSnapshot> result = ConfigurationResolver::Resolve(schema, request, 9);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        };
        ConfigurationResolutionRequest request;
        request.packagedProfile.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{2}, "profile"));
        REQUIRE(std::get<std::int64_t>(resolve(request).Get(SettingKey{"runtime.worker_count"})) == 2);
        request.user.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{3}, "user.json"));
        REQUIRE(std::get<std::int64_t>(resolve(request).Get(SettingKey{"runtime.worker_count"})) == 3);
        request.project.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{4}, "project.json"));
        REQUIRE(std::get<std::int64_t>(resolve(request).Get(SettingKey{"runtime.worker_count"})) == 4);
        request.session.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{5}, "session"));
        REQUIRE(std::get<std::int64_t>(resolve(request).Get(SettingKey{"runtime.worker_count"})) == 5);
        request.environment.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{6}, "HORO_WORKERS"));
        REQUIRE(resolve(request).FindResolved(SettingKey{"runtime.worker_count"})->source == ConfigurationSource::Environment);
        request.invocation.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{7}, "--workers"));
        const ConfigurationSnapshot snapshot = resolve(request);
        REQUIRE(snapshot.Revision() == 9);
        REQUIRE(std::get<std::int64_t>(snapshot.Get(SettingKey{"runtime.worker_count"})) == 7);
        REQUIRE(snapshot.FindResolved(SettingKey{"runtime.worker_count"})->source == ConfigurationSource::Invocation);
        REQUIRE(snapshot.FindResolved(SettingKey{"runtime.worker_count"})->location->source == "--workers");
    }

    TEST_CASE("Session Override Policy Is The Only Environment Precedence Exception", "[unit][foundation][configuration]") {
        const ConfigurationSchema schema = BuildSchema(true);
        ConfigurationResolutionRequest request;
        request.environment.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{6}, "HORO_WORKERS"));
        request.session.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{8}, "preview"));
        Result<ConfigurationSnapshot> result = ConfigurationResolver::Resolve(schema, request);
        REQUIRE(result.HasValue());
        REQUIRE(std::get<std::int64_t>(result.Value().Get(SettingKey{"runtime.worker_count"})) == 8);
        REQUIRE(result.Value().FindResolved(SettingKey{"runtime.worker_count"})->source == ConfigurationSource::Session);
    }

    TEST_CASE("Resolver Rejects All Invalid Sources In Deterministic Order", "[unit][foundation][configuration]") {
        ConfigurationSchema schema;
        SettingDescriptor projectOnly = ResolutionDescriptor();
        projectOnly.sourcePolicy = ConfigurationSourcePolicy{.allowedSources = ConfigurationSourceMask::Project};
        REQUIRE(schema.Register(projectOnly).HasValue());
        REQUIRE(schema.Seal().HasValue());

        ConfigurationResolutionRequest request;
        request.invocation.try_emplace(SettingKey{"z.unknown"}, Input(true, "arg-z"));
        request.invocation.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{4}, "arg-workers"));
        request.project.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::string{"four"}, "project.json"));
        Result<ConfigurationSnapshot> result = ConfigurationResolver::Resolve(schema, request);
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == "configuration.resolution_failed");
        REQUIRE(result.ErrorValue().diagnostics.size() == 3);
        REQUIRE(result.ErrorValue().diagnostics[0].code.Value() == "configuration.source_forbidden");
        REQUIRE(result.ErrorValue().diagnostics[1].code.Value() == "configuration.unknown_key");
        REQUIRE(result.ErrorValue().diagnostics[2].code.Value() == "configuration.value_invalid");
    }

    TEST_CASE("Failed Multi Source Resolution Activates Nothing", "[unit][foundation][configuration]") {
        ConfigurationService service{BuildSchema()};
        const ConfigurationSnapshot before = service.Snapshot();
        ConfigurationResolutionRequest request;
        request.project.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{12}, "project.json"));
        request.user.try_emplace(SettingKey{"unknown.setting"}, Input(true, "user.json"));
        REQUIRE(service.ResolveAndCommit(request).HasError());
        REQUIRE(service.Snapshot().Revision() == before.Revision());
        REQUIRE(std::get<std::int64_t>(service.Snapshot().Get(SettingKey{"runtime.worker_count"})) == 1);
    }

    TEST_CASE("Environment Capture Uses Only The Injected Host Provider", "[unit][foundation][configuration]") {
        const ConfigurationSchema schema = BuildSchema();
        FixedProcessService processes;
        processes.values["HORO_WORKERS"] = "16";
        const std::vector bindings{EnvironmentVariableBinding{.key = SettingKey{"runtime.worker_count"}, .variable = "HORO_WORKERS"}};
        Result<ConfigurationSourceMap> captured = ConfigurationResolver::CaptureEnvironment(schema, bindings, processes);
        REQUIRE(captured.HasValue());
        REQUIRE(std::get<std::int64_t>(captured.Value().at(SettingKey{"runtime.worker_count"}).value) == 16);
        REQUIRE(captured.Value().at(SettingKey{"runtime.worker_count"}).location->source == "HORO_WORKERS");

        processes.values["HORO_WORKERS"] = "16garbage";
        REQUIRE(ConfigurationResolver::CaptureEnvironment(schema, bindings, processes).HasError());
        processes.values["HORO_WORKERS"] = "";
        REQUIRE(ConfigurationResolver::CaptureEnvironment(schema, bindings, processes).HasError());
        const std::vector invalid{EnvironmentVariableBinding{.key = SettingKey{"runtime.worker_count"}, .variable = "WORKERS"}};
        REQUIRE(ConfigurationResolver::CaptureEnvironment(schema, invalid, processes).HasError());
    }

    TEST_CASE("Secret Markers Reject External Values And Never Serialize Defaults", "[unit][foundation][configuration]") {
        ConfigurationSchema schema;
        REQUIRE(schema
                    .Register({.key = SettingKey{"release.signing.reference"},
                               .type = SettingValueType::String,
                               .defaultValue = std::string{"provider-reference"},
                               .scope = SettingScope::User,
                               .reloadPolicy = ReloadPolicy::NextOperation,
                               .sensitivity = SettingSensitivity::SecretReference})
                    .HasValue());
        REQUIRE(schema.Seal().HasValue());
        Result<ConfigurationSnapshot> defaults = ConfigurationResolver::Resolve(schema, {});
        REQUIRE(defaults.HasValue());
        REQUIRE(defaults.Value().ToJson().find("provider-reference") == std::string::npos);

        ConfigurationResolutionRequest request;
        request.user.try_emplace(SettingKey{"release.signing.reference"}, Input(std::string{"raw-secret"}, "user.json"));
        Result<ConfigurationSnapshot> rejected = ConfigurationResolver::Resolve(schema, request);
        REQUIRE(rejected.HasError());
        REQUIRE(rejected.ErrorValue().diagnostics.front().code.Value() == "configuration.secret_value_forbidden");
        REQUIRE(rejected.ErrorValue().diagnostics.front().message.find("raw-secret") == std::string::npos);
    }

    TEST_CASE("Versioned Documents Are Strict Bounded And Deterministic", "[unit][foundation][configuration]") {
        ConfigurationSchema schema;
        REQUIRE(schema.Register(kThemeDescriptor).HasValue());
        REQUIRE(schema.Register(kAutosaveDescriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        const std::string document =
            R"json({"schemaVersion":1,"values":{"editor.theme.active":"solarized","editor.autosave.enabled":false}})json";
        Result<ConfigurationSourceMap> parsed = ConfigurationResolver::ParseDocument(schema, document, "user.json");
        REQUIRE(parsed.HasValue());
        REQUIRE(parsed.Value().size() == 2);
        REQUIRE(ConfigurationResolver::ParseDocument(schema, R"json({"values":{}})json", "user.json").HasError());
        REQUIRE(ConfigurationResolver::ParseDocument(schema, R"json({"schemaVersion":2,"values":{}})json", "user.json").HasError());
        REQUIRE(
            ConfigurationResolver::
                ParseDocument(schema,
                              R"json({"schemaVersion":1,"values":{"editor.autosave.enabled":true,"editor.autosave.enabled":false}})json",
                              "user.json")
                    .HasError());
        REQUIRE(ConfigurationResolver::ParseDocument(schema, R"json({"schemaVersion":1,"values":{},"extra":true})json", "user.json")
                    .HasError());
        Result<ConfigurationSourceMap> unknown =
            ConfigurationResolver::ParseDocument(schema, R"json({"schemaVersion":1,"values":{"unknown.setting":true}})json", "user.json");
        REQUIRE(unknown.HasValue());
        ConfigurationResolutionRequest unknownRequest;
        unknownRequest.user = std::move(unknown).Value();
        REQUIRE(ConfigurationResolver::Resolve(schema, unknownRequest).HasError());
        ConfigurationLimits tiny;
        tiny.maximumDocumentBytes = 8;
        REQUIRE(ConfigurationResolver::ParseDocument(schema, document, "user.json", tiny).HasError());

        ConfigurationService service = BuildService();
        REQUIRE(service.LoadJson(document).HasValue());
        const std::string serialized = service.Snapshot().ToJson();
        REQUIRE(serialized.find("editor.autosave.enabled") < serialized.find("editor.theme.active"));
        REQUIRE(serialized == service.Snapshot().ToJson());
        REQUIRE(service.LoadFile("must-not-be-read.json").HasError());
        REQUIRE(service.SaveFile("must-not-be-written.json").HasError());
    }

    TEST_CASE("Concurrent Readers Retain Whole Snapshot Revisions", "[unit][foundation][configuration]") {
        ConfigurationService service{BuildSchema()};
        const ConfigurationSnapshot captured = service.Snapshot();
        std::atomic<bool> start{false};
        std::atomic<bool> invalid{false};
        std::vector<std::thread> readers;
        for (int reader = 0; reader < 4; ++reader) {
            readers.emplace_back([&] {
                while (!start.load()) {
                }
                for (int iteration = 0; iteration < 500; ++iteration) {
                    const ConfigurationSnapshot snapshot = service.Snapshot();
                    const auto value = std::get<std::int64_t>(snapshot.Get(SettingKey{"runtime.worker_count"}));
                    if ((snapshot.Revision() == 0 && value != 1) || (snapshot.Revision() == 1 && value != 32))
                        invalid.store(true);
                }
            });
        }
        start.store(true);
        ConfigurationResolutionRequest request;
        request.invocation.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{32}, "--workers"));
        REQUIRE(service.ResolveAndCommit(request).HasValue());
        for (std::thread &reader : readers)
            reader.join();
        REQUIRE_FALSE(invalid.load());
        REQUIRE(captured.Revision() == 0);
        REQUIRE(std::get<std::int64_t>(captured.Get(SettingKey{"runtime.worker_count"})) == 1);
    }

    TEST_CASE("Commit Rejects Stale Draft And Validation Does Not Mutate", "[unit][foundation][configuration]") {
        ConfigurationService service = BuildService();
        ConfigurationDraft draft{.baseRevision = 8};
        draft.proposedValues.try_emplace(SettingKey{"editor.theme.active"}, std::string{"light"});
        REQUIRE(service.Validate(draft).HasError());
        REQUIRE(service.Commit(draft).HasError());
        REQUIRE(service.Snapshot().Revision() == 0);

        ConfigurationDraft oversized{.baseRevision = 0};
        oversized.proposedValues.try_emplace(SettingKey{"editor.theme.active"}, std::string(64 * 1024 + 1, 'x'));
        REQUIRE(oversized.proposedValues.begin()->second.index() == 2);
        REQUIRE(service.Validate(oversized).HasError());
        REQUIRE(service.Commit(oversized).HasError());
        REQUIRE(service.Snapshot().Revision() == 0);
    }

    TEST_CASE("Commit Publishes Sorted Bounded Change Metadata", "[unit][foundation][configuration]") {
        EngineDataBus events{EngineDataBusConfig{.traceDispatch = false}};
        ConfigurationSchema schema;
        REQUIRE(schema.Register(kThemeDescriptor).HasValue());
        REQUIRE(schema.Register(kAutosaveDescriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationService service{std::move(schema), &events};

        ConfigurationChangedEvent observed{};
        const Subscription subscription = events.Subscribe<ConfigurationChangedEvent>([&observed](const auto &event) {
            observed = event;
        });
        ConfigurationDraft draft{.baseRevision = 0};
        draft.proposedValues.try_emplace(SettingKey{"editor.theme.active"}, std::string{"light"});
        draft.proposedValues.try_emplace(SettingKey{"editor.autosave.enabled"}, false);
        REQUIRE(service.Commit(draft).HasValue());
        REQUIRE(observed.revision == 1);
        REQUIRE(observed.changedKeys.size() == 2);
        REQUIRE(observed.changedKeys[0].Value() == "editor.autosave.enabled");
        REQUIRE(observed.changedKeys[1].Value() == "editor.theme.active");
    }

    TEST_CASE("Reload Storm Commits Latest Valid Snapshot Once On Owner Dispatch", "[unit][foundation][configuration]") {
        EngineDataBus events{EngineDataBusConfig{.traceDispatch = false}};
        ConfigurationSchema schema;
        REQUIRE(schema.Register(kThemeDescriptor).HasValue());
        REQUIRE(schema.Register(kAutosaveDescriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationService service{std::move(schema), &events};
        std::vector<ConfigurationChangedEvent> observed;
        std::vector<std::thread::id> deliveryThreads;
        bool mismatchedRevision = false;
        const Subscription subscription = events.Subscribe<ConfigurationChangedEvent>([&](const auto &event) {
            observed.push_back(event);
            deliveryThreads.push_back(std::this_thread::get_id());
            mismatchedRevision |= service.Snapshot().Revision() != event.revision;
        });

        const ConfigurationSnapshot captured = service.Snapshot();
        std::atomic<bool> producerFailed{false};
        std::thread producer([&] {
            for (int iteration = 0; iteration < 100; ++iteration) {
                ConfigurationResolutionRequest request;
                request.user.try_emplace(SettingKey{"editor.theme.active"}, Input(std::string{iteration == 99 ? "light" : "dark"}, "user"));
                request.user.try_emplace(SettingKey{"editor.autosave.enabled"}, Input(false, "user"));
                if (service.StageReload(request).HasError())
                    producerFailed.store(true);
            }
            if (const auto activated = service.ActivateReload(ConfigurationReloadPoint::NextFrame);
                activated.HasError() || !activated.Value())
                producerFailed.store(true);
        });
        producer.join();
        REQUIRE_FALSE(producerFailed.load());
        REQUIRE(observed.empty());
        REQUIRE(service.Snapshot().Revision() == 1);
        REQUIRE(captured.Revision() == 0);
        REQUIRE(events.QueueStats().enqueued == 1);
        REQUIRE(events.QueueStats().droppedNewest == 0);
        events.DispatchQueued();
        REQUIRE(observed.size() == 1);
        REQUIRE_FALSE(mismatchedRevision);
        REQUIRE(observed.front().changedKeys.size() == 2);
        REQUIRE(deliveryThreads.front() == std::this_thread::get_id());
        REQUIRE(std::get<std::string>(service.Snapshot().Get(SettingKey{"editor.theme.active"})) == "light");
        REQUIRE_FALSE(service.ActivateReload(ConfigurationReloadPoint::NextFrame).Value());
    }

    TEST_CASE("Reload Of Unchanged Values Does Not Notify", "[unit][foundation][configuration]") {
        EngineDataBus events{EngineDataBusConfig{.traceDispatch = false}};
        ConfigurationSchema schema;
        REQUIRE(schema.Register(kThemeDescriptor).HasValue());
        REQUIRE(schema.Register(kAutosaveDescriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationService service{std::move(schema), &events};
        int notifications = 0;
        const Subscription subscription = events.Subscribe<ConfigurationChangedEvent>([&](const auto &) {
            ++notifications;
        });
        ConfigurationResolutionRequest unchanged;
        unchanged.user.try_emplace(SettingKey{"editor.theme.active"}, Input(std::string{"light"}, "user"));
        unchanged.user.try_emplace(SettingKey{"editor.autosave.enabled"}, Input(false, "user"));
        REQUIRE(service.StageReload(unchanged).HasValue());
        REQUIRE(service.ActivateReload(ConfigurationReloadPoint::NextFrame).Value());
        events.DispatchQueued();
        REQUIRE(notifications == 1);
        REQUIRE(service.StageReload(unchanged).HasValue());
        REQUIRE_FALSE(service.ActivateReload(ConfigurationReloadPoint::NextFrame).Value());
        events.DispatchQueued();
        REQUIRE(notifications == 1);
        REQUIRE(service.Snapshot().Revision() == 1);
    }

    TEST_CASE("Reload Rejects Whole Invalid Candidate And Cancels Pending Input", "[unit][foundation][configuration]") {
        EngineDataBus events{EngineDataBusConfig{.traceDispatch = false}};
        ConfigurationSchema schema;
        REQUIRE(schema.Register(kThemeDescriptor).HasValue());
        REQUIRE(schema.Register(kAutosaveDescriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationService service{std::move(schema), &events};
        int notifications = 0;
        const Subscription subscription = events.Subscribe<ConfigurationChangedEvent>([&](const auto &) {
            ++notifications;
        });
        ConfigurationResolutionRequest valid;
        valid.user.try_emplace(SettingKey{"editor.theme.active"}, Input(std::string{"light"}, "user"));
        REQUIRE(service.StageReload(valid).HasValue());
        ConfigurationResolutionRequest invalid = valid;
        invalid.user.try_emplace(SettingKey{"editor.autosave.enabled"}, Input(std::string{"invalid"}, "user"));
        const auto rejected = service.StageReload(invalid);
        REQUIRE(rejected.HasError());
        REQUIRE_FALSE(rejected.ErrorValue().diagnostics.empty());
        REQUIRE_FALSE(service.ActivateReload(ConfigurationReloadPoint::NextFrame).Value());
        events.DispatchQueued();
        REQUIRE(notifications == 0);
        REQUIRE(service.Snapshot().Revision() == 0);
    }

    TEST_CASE("Malformed Reload Document Cancels Earlier Staged Candidate", "[unit][foundation][configuration]") {
        EngineDataBus events{EngineDataBusConfig{.traceDispatch = false}};
        ConfigurationSchema schema;
        REQUIRE(schema.Register(kThemeDescriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationService service{schema, &events};
        ConfigurationResolutionRequest valid;
        valid.user.try_emplace(SettingKey{"editor.theme.active"}, Input(std::string{"light"}, "user"));
        REQUIRE(service.StageReload(valid).HasValue());

        const auto malformed = ConfigurationResolver::ParseDocument(schema, "{", "user.json");
        REQUIRE(malformed.HasError());
        REQUIRE_FALSE(malformed.ErrorValue().diagnostics.empty());
        service.CancelPendingReload();

        REQUIRE_FALSE(service.ActivateReload(ConfigurationReloadPoint::NextFrame).Value());
        REQUIRE(service.Snapshot().Revision() == 0);
        REQUIRE(events.QueueStats().enqueued == 0);
    }

    TEST_CASE("Reload Waits For Every Descriptor Policy And Invalidates On Direct Commit", "[unit][foundation][configuration]") {
        ConfigurationSchema schema;
        REQUIRE(schema.Register(kThemeDescriptor).HasValue());
        SettingDescriptor reopen = kAutosaveDescriptor;
        reopen.reloadPolicy = ReloadPolicy::ProjectReopen;
        REQUIRE(schema.Register(reopen).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationService service{std::move(schema)};
        ConfigurationResolutionRequest request;
        request.user.try_emplace(SettingKey{"editor.theme.active"}, Input(std::string{"light"}, "user"));
        request.user.try_emplace(SettingKey{"editor.autosave.enabled"}, Input(false, "user"));
        REQUIRE(service.StageReload(request).HasValue());
        REQUIRE_FALSE(service.ActivateReload(ConfigurationReloadPoint::Immediate).Value());
        REQUIRE_FALSE(service.ActivateReload(ConfigurationReloadPoint::NextFrame).Value());
        REQUIRE(service.Snapshot().Revision() == 0);
        REQUIRE(service.ActivateReload(ConfigurationReloadPoint::ProjectReopen).Value());
        REQUIRE(service.Snapshot().Revision() == 1);

        REQUIRE(service.StageReload({}).HasValue());
        ConfigurationDraft direct{.baseRevision = 1};
        direct.proposedValues.try_emplace(SettingKey{"editor.theme.active"}, std::string{"dark"});
        REQUIRE(service.Commit(direct).HasValue());
        REQUIRE_FALSE(service.ActivateReload(ConfigurationReloadPoint::ProcessRestart).Value());
        REQUIRE(service.Snapshot().Revision() == 2);
    }

    TEST_CASE("Process Restart Policy Defers Activation And Equal Reload Is Silent", "[unit][foundation][configuration]") {
        ConfigurationSchema schema;
        SettingDescriptor restart = ResolutionDescriptor();
        restart.reloadPolicy = ReloadPolicy::ProcessRestart;
        REQUIRE(schema.Register(restart).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationService service{std::move(schema)};
        REQUIRE(service.StageReload({}).HasValue());
        REQUIRE_FALSE(service.ActivateReload(ConfigurationReloadPoint::ProcessRestart).Value());
        REQUIRE(service.Snapshot().Revision() == 0);
        ConfigurationResolutionRequest request;
        request.invocation.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{4}, "--workers"));
        REQUIRE(service.StageReload(request).HasValue());
        REQUIRE_FALSE(service.ActivateReload(ConfigurationReloadPoint::ProjectReopen).Value());
        REQUIRE(service.Snapshot().Revision() == 0);
        REQUIRE(service.ActivateReload(ConfigurationReloadPoint::ProcessRestart).Value());
        REQUIRE(service.Snapshot().Revision() == 1);
    }

    TEST_CASE("Every Reload Policy Requires Its Host Owned Boundary", "[unit][foundation][configuration]") {
        const auto check = [](const ReloadPolicy policy, const std::vector<ConfigurationReloadPoint> &early,
                              const ConfigurationReloadPoint allowed) {
            ConfigurationSchema schema;
            SettingDescriptor descriptor = kAutosaveDescriptor;
            descriptor.reloadPolicy = policy;
            REQUIRE(schema.Register(descriptor).HasValue());
            REQUIRE(schema.Seal().HasValue());
            ConfigurationService service{std::move(schema)};
            ConfigurationResolutionRequest request;
            request.user.try_emplace(SettingKey{"editor.autosave.enabled"}, Input(false, "user"));
            REQUIRE(service.StageReload(request).HasValue());
            for (const ConfigurationReloadPoint point : early) {
                REQUIRE_FALSE(service.ActivateReload(point).Value());
                REQUIRE(service.Snapshot().Revision() == 0);
            }
            REQUIRE(service.ActivateReload(allowed).Value());
            REQUIRE(service.Snapshot().Revision() == 1);
        };
        using enum ConfigurationReloadPoint;
        check(ReloadPolicy::Immediate, {}, Immediate);
        check(ReloadPolicy::NextFrame, {Immediate, NextOperation}, NextFrame);
        check(ReloadPolicy::NextOperation, {Immediate, NextFrame}, NextOperation);
        check(ReloadPolicy::ProjectReopen, {Immediate, NextFrame, NextOperation, NextFrameAndOperation}, ProjectReopen);
        check(ReloadPolicy::ProcessRestart, {Immediate, NextFrame, NextOperation, NextFrameAndOperation, ProjectReopen}, ProcessRestart);

        ConfigurationSchema mixedSchema;
        REQUIRE(mixedSchema.Register(kThemeDescriptor).HasValue());
        SettingDescriptor operation = kAutosaveDescriptor;
        operation.reloadPolicy = ReloadPolicy::NextOperation;
        REQUIRE(mixedSchema.Register(operation).HasValue());
        REQUIRE(mixedSchema.Seal().HasValue());
        ConfigurationService mixed{std::move(mixedSchema)};
        ConfigurationResolutionRequest request;
        request.user.try_emplace(SettingKey{"editor.theme.active"}, Input(std::string{"light"}, "user"));
        request.user.try_emplace(SettingKey{"editor.autosave.enabled"}, Input(false, "user"));
        REQUIRE(mixed.StageReload(request).HasValue());
        REQUIRE_FALSE(mixed.ActivateReload(NextFrame).Value());
        REQUIRE_FALSE(mixed.ActivateReload(NextOperation).Value());
        REQUIRE(mixed.ActivateReload(NextFrameAndOperation).Value());
    }

    TEST_CASE("Invalid Reload Storm Cancels Pending Candidate And Reports Diagnostics", "[unit][foundation][configuration]") {
        EngineDataBus events{EngineDataBusConfig{.traceDispatch = false}};
        ConfigurationService service{BuildSchema(), &events};
        int notifications = 0;
        const Subscription subscription = events.Subscribe<ConfigurationChangedEvent>([&](const auto &) {
            ++notifications;
        });
        ConfigurationResolutionRequest valid;
        valid.invocation.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::int64_t{3}, "--workers"));
        for (int iteration = 0; iteration < 50; ++iteration) {
            REQUIRE(service.StageReload(valid).HasValue());
            ConfigurationResolutionRequest invalid = valid;
            invalid.user.try_emplace(SettingKey{"runtime.worker_count"}, Input(std::string{"wrong-type"}, "user.json"));
            const auto rejected = service.StageReload(invalid);
            REQUIRE(rejected.HasError());
            REQUIRE_FALSE(rejected.ErrorValue().diagnostics.empty());
        }
        REQUIRE_FALSE(service.ActivateReload(ConfigurationReloadPoint::NextOperation).Value());
        events.DispatchQueued();
        REQUIRE(notifications == 0);
        REQUIRE(service.Snapshot().Revision() == 0);
        REQUIRE(service.StageReload(valid).HasValue());
        REQUIRE(service.ActivateReload(ConfigurationReloadPoint::NextOperation).Value());
        events.DispatchQueued();
        REQUIRE(notifications == 1);
    }

    TEST_CASE("Queued Reload Events Stay Snapshot Correlated When Direct Commits Deliver First", "[unit][foundation][configuration]") {
        EngineDataBus events{EngineDataBusConfig{.traceDispatch = false}};
        ConfigurationSchema schema;
        REQUIRE(schema.Register(kThemeDescriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationService service{std::move(schema), &events};
        std::vector<ConfigurationRevision> delivered;
        bool eventAheadOfSnapshot = false;
        const Subscription subscription = events.Subscribe<ConfigurationChangedEvent>([&](const auto &event) {
            delivered.push_back(event.revision);
            eventAheadOfSnapshot |= service.Snapshot().Revision() < event.revision;
        });
        ConfigurationResolutionRequest reloaded;
        reloaded.user.try_emplace(SettingKey{"editor.theme.active"}, Input(std::string{"light"}, "user"));
        REQUIRE(service.StageReload(reloaded).HasValue());
        REQUIRE(service.ActivateReload(ConfigurationReloadPoint::NextFrame).Value());
        const ConfigurationSnapshot first = service.Snapshot();
        REQUIRE(first.Revision() == 1);

        ConfigurationResolutionRequest direct;
        direct.user.try_emplace(SettingKey{"editor.theme.active"}, Input(std::string{"dark"}, "user"));
        REQUIRE(service.ResolveAndCommit(direct).HasValue());
        const std::vector<ConfigurationRevision> beforeDispatch{2};
        REQUIRE(delivered == beforeDispatch);
        events.DispatchQueued();
        const std::vector<ConfigurationRevision> afterDispatch{2, 1};
        REQUIRE(delivered == afterDispatch);
        REQUIRE_FALSE(eventAheadOfSnapshot);
        REQUIRE(first.Revision() == 1);
        REQUIRE(std::get<std::string>(first.Get(SettingKey{"editor.theme.active"})) == "light");
        REQUIRE(std::get<std::string>(service.Snapshot().Get(SettingKey{"editor.theme.active"})) == "dark");
    }

    TEST_CASE("Concurrent Reload Staging And Snapshot Readers See Whole Revisions", "[unit][foundation][configuration]") {
        ConfigurationSchema schema;
        REQUIRE(schema.Register(kThemeDescriptor).HasValue());
        REQUIRE(schema.Register(kAutosaveDescriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationService service{std::move(schema)};
        std::atomic<bool> done{false};
        std::atomic<bool> inconsistent{false};
        std::thread producer([&] {
            for (int iteration = 0; iteration < 100; ++iteration) {
                ConfigurationResolutionRequest request;
                request.user.try_emplace(SettingKey{"editor.theme.active"},
                                         Input(std::string{iteration % 2 == 0 ? "light" : "dark"}, "user"));
                request.user.try_emplace(SettingKey{"editor.autosave.enabled"}, Input(false, "user"));
                if (service.StageReload(request).HasError())
                    inconsistent.store(true);
            }
        });
        std::thread reader([&] {
            while (!done.load()) {
                const ConfigurationSnapshot snapshot = service.Snapshot();
                const auto theme = std::get<std::string>(snapshot.Get(SettingKey{"editor.theme.active"}));
                const bool autosave = std::get<bool>(snapshot.Get(SettingKey{"editor.autosave.enabled"}));
                if ((snapshot.Revision() == 0 && (theme != "midnight" || !autosave)) ||
                    (snapshot.Revision() != 0 && (theme == "midnight" || autosave)))
                    inconsistent.store(true);
            }
        });
        producer.join();
        REQUIRE(service.ActivateReload(ConfigurationReloadPoint::NextFrame).Value());
        done.store(true);
        reader.join();
        REQUIRE_FALSE(inconsistent.load());
        REQUIRE(service.Snapshot().Revision() == 1);
    }

    TEST_CASE("Racing Valid And Invalid Reloads Never Activate Partial Input", "[unit][foundation][configuration]") {
        ConfigurationService service = BuildService();
        std::atomic<bool> start{false};
        ConfigurationResolutionRequest valid;
        valid.user.try_emplace(SettingKey{"editor.theme.active"}, Input(std::string{"light"}, "user"));
        valid.user.try_emplace(SettingKey{"editor.autosave.enabled"}, Input(false, "user"));
        ConfigurationResolutionRequest invalid = valid;
        invalid.user[SettingKey{"editor.autosave.enabled"}] = Input(std::string{"bad"}, "user");
        std::thread first([&] {
            while (!start.load())
                std::this_thread::yield();
            static_cast<void>(service.StageReload(valid));
        });
        std::thread second([&] {
            while (!start.load())
                std::this_thread::yield();
            static_cast<void>(service.StageReload(invalid));
        });
        start.store(true);
        first.join();
        second.join();
        const auto activation = service.ActivateReload(ConfigurationReloadPoint::NextFrame);
        REQUIRE(activation.HasValue());
        const ConfigurationSnapshot snapshot = service.Snapshot();
        if (activation.Value()) {
            REQUIRE(snapshot.Revision() == 1);
            REQUIRE(std::get<std::string>(snapshot.Get(SettingKey{"editor.theme.active"})) == "light");
            REQUIRE_FALSE(std::get<bool>(snapshot.Get(SettingKey{"editor.autosave.enabled"})));
        } else {
            REQUIRE(snapshot.Revision() == 0);
            REQUIRE(std::get<std::string>(snapshot.Get(SettingKey{"editor.theme.active"})) == "midnight");
            REQUIRE(std::get<bool>(snapshot.Get(SettingKey{"editor.autosave.enabled"})));
        }
    }
}  // namespace
