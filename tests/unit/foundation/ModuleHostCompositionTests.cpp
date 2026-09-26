#include "Horo/Foundation/ModuleHost.h"
#include "ModuleDescriptorTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
    using namespace Horo;
    using Horo::Test::MakeModule;

    // Module lifecycle callbacks are plain function addresses copied into
    // descriptors, so every piece of state they touch is file-local and outlives
    // all hosts in this translation unit. Each test resets the log first.
    struct ActivationLog {
        std::vector<std::string> activated;
        std::vector<std::string> drained;
        std::vector<std::string> deactivated;
        std::vector<ModuleActivationContext::DependencyBindings> bindings;
        std::vector<bool> drainObservedCancellation;
        std::vector<bool> drainObservedClosedAdmission;
    };

    ActivationLog g_log;
    int g_instancesAlive = 0;
    std::string g_failModule;
    // Modules observed entering the active set; the rollback order source of truth.
    std::vector<std::string> g_activationCompletions;

    void ResetLog() {
        g_log.activated.clear();
        g_log.drained.clear();
        g_log.deactivated.clear();
        g_log.bindings.clear();
        g_log.drainObservedCancellation.clear();
        g_log.drainObservedClosedAdmission.clear();
        g_instancesAlive = 0;
        g_failModule.clear();
        g_activationCompletions.clear();
    }

    /** @brief Instance type proving attached module state is released with its context. */
    struct CountingInstance final : IModuleInstance {
        CountingInstance() {
            ++g_instancesAlive;
        }

        ~CountingInstance() override {
            --g_instancesAlive;
        }
    };

    Result<void> LogActivate(ModuleActivationContext &context) noexcept {
        g_log.activated.push_back(context.Module().value);
        g_log.bindings.push_back(context.Bindings());
        if (g_failModule == context.Module().value)
            return Result<void>::Failure(Error{});
        (void)context.AttachInstance(std::make_unique<CountingInstance>());
        g_activationCompletions.push_back(context.Module().value);
        return Result<void>::Success();
    }

    void LogDeactivate(ModuleActivationContext &context) noexcept {
        g_log.deactivated.push_back(context.Module().value);
    }

    void LogDrain(ModuleActivationContext &context) noexcept {
        g_log.drained.push_back(context.Module().value);
        g_log.drainObservedCancellation.push_back(context.Cancellation().IsCancellationRequested());
        g_log.drainObservedClosedAdmission.push_back(!context.AcquireCallbackLease().has_value());
    }

    [[nodiscard]] ModuleDescriptor MakeLoggedModule(std::string id, const ModuleContractVersion version = {1, 0, 0}) {
        ModuleDescriptor descriptor = MakeModule(std::move(id), version);
        descriptor.lifecycle = ModuleLifecycleCallbacks{.activate = &LogActivate, .drain = &LogDrain, .deactivate = &LogDeactivate};
        return descriptor;
    }

    [[nodiscard]] ModuleConfigurationContribution Settings(std::string module, std::string owner, std::string key,
                                                           std::string variable = {}) {
        ModuleConfigurationContribution contribution{.module = ModuleId{std::move(module)},
                                                     .ownerPrefix = std::move(owner),
                                                     .settings = {
                                                         SettingDescriptor{.key = SettingKey{std::move(key)},
                                                                           .type = SettingValueType::Integer,
                                                                           .defaultValue = std::int64_t{42},
                                                                           .scope = SettingScope::Engine,
                                                                           .reloadPolicy = ReloadPolicy::NextOperation,
                                                                           .sensitivity = SettingSensitivity::Public,
                                                                           .sourcePolicy = ConfigurationSourcePolicy{
                                                                               .allowedSources = ConfigurationSourceMask::Invocation |
                                                                                                 ConfigurationSourceMask::Environment}}}};
        if (!variable.empty())
            contribution.environmentBindings.push_back({.key = contribution.settings.front().key, .variable = std::move(variable)});
        return contribution;
    }
}  // namespace

TEST_CASE("Module settings registration reports typed conflicts without partial registration",
          "[unit][foundation][modules][configuration]") {
    ModuleHost host;
    const auto first = Settings("horo.alpha", "alpha", "alpha.count", "HORO_ALPHA_COUNT");
    REQUIRE(host.Register(MakeModule("horo.alpha"), first).HasValue());

    auto duplicate = Settings("horo.beta", "alpha", "alpha.count");
    auto result = host.Register(MakeModule("horo.beta"), duplicate);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == "foundation.module.duplicate_setting");
    CHECK_FALSE(host.StateOf(ModuleId{"horo.beta"}).has_value());

    auto overlap = Settings("horo.beta", "alpha.child", "alpha.child.count");
    result = host.Register(MakeModule("horo.beta"), overlap);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == "foundation.module.setting_owner_conflict");

    auto binding = Settings("horo.beta", "beta", "beta.count", "HORO_ALPHA_COUNT");
    result = host.Register(MakeModule("horo.beta"), binding);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == "foundation.module.duplicate_environment_binding");

    binding.environmentBindings.front().variable = "HORO_BETA_COUNT";
    REQUIRE(host.Register(MakeModule("horo.beta"), binding).HasValue());
}

TEST_CASE("Module settings reject invalid local metadata without registering the module", "[unit][foundation][modules][configuration]") {
    ModuleHost host;
    auto contribution = Settings("horo.alpha", "alpha", "alpha.count", "HORO_ALPHA_COUNT");
    contribution.settings.front().sourcePolicy->allowedSources = ConfigurationSourceMask::Invocation;
    auto result = host.Register(MakeModule("horo.alpha"), contribution);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == "foundation.module.invalid_settings_contribution");
    CHECK_FALSE(host.StateOf(ModuleId{"horo.alpha"}).has_value());

    contribution.settings.front().sourcePolicy->allowedSources = ConfigurationSourceMask::Invocation | ConfigurationSourceMask::Environment;
    contribution.environmentBindings.push_back({.key = contribution.settings.front().key, .variable = "HORO_ALPHA_OTHER"});
    result = host.Register(MakeModule("horo.alpha"), contribution);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == "foundation.module.invalid_settings_contribution");
    CHECK_FALSE(host.StateOf(ModuleId{"horo.alpha"}).has_value());

    contribution.environmentBindings.pop_back();
    REQUIRE(host.Register(MakeModule("horo.alpha"), contribution).HasValue());
}

TEST_CASE("Module settings resolve in stable host order and captured snapshots survive shutdown",
          "[unit][foundation][modules][configuration]") {
    const auto makeHost = [](const bool reverse) {
        auto host = std::make_unique<ModuleHost>();
        const auto registerOne = [&](const std::string &id, const std::string &owner) {
            REQUIRE(host->Register(MakeModule(id), Settings(id, owner, owner + ".count",
                                                            "HORO_" + std::string{id == "horo.alpha" ? "ALPHA" : "BETA"} + "_COUNT"))
                        .HasValue());
        };
        if (reverse) {
            registerOne("horo.beta", "beta");
            registerOne("horo.alpha", "alpha");
        } else {
            registerOne("horo.alpha", "alpha");
            registerOne("horo.beta", "beta");
        }
        REQUIRE(host->BuildConfigurationSchema().Value().FindDescriptor(SettingKey{"alpha.count"}) == nullptr);
        REQUIRE(host->ActivateRegistered(nullptr).HasValue());
        return host;
    };
    auto first = makeHost(false);
    auto second = makeHost(true);
    CHECK(first->ConfigurationEnvironmentBindings()[0].variable == "HORO_ALPHA_COUNT");
    CHECK(first->ConfigurationEnvironmentBindings()[1].variable == "HORO_BETA_COUNT");
    CHECK(first->ConfigurationEnvironmentBindings()[0].variable == second->ConfigurationEnvironmentBindings()[0].variable);
    CHECK(first->ConfigurationEnvironmentBindings()[1].variable == second->ConfigurationEnvironmentBindings()[1].variable);

    auto schema = first->BuildConfigurationSchema();
    REQUIRE(schema.HasValue());
    ConfigurationSchema ownedSchema = std::move(schema).Value();
    REQUIRE(ownedSchema.FindDescriptor(SettingKey{"alpha.count"}) != nullptr);
    REQUIRE(ownedSchema.Seal().HasValue());
    ConfigurationResolutionRequest request;
    request.invocation.try_emplace(SettingKey{"beta.count"}, ConfigurationInputValue{.value = std::int64_t{7}});
    auto resolved = ConfigurationResolver::Resolve(ownedSchema, request);
    REQUIRE(resolved.HasValue());
    const ConfigurationSnapshot captured = resolved.Value();
    CHECK(std::get<std::int64_t>(captured.Get(SettingKey{"alpha.count"})) == 42);
    CHECK(std::get<std::int64_t>(captured.Get(SettingKey{"beta.count"})) == 7);

    first->DeactivateAll();
    CHECK_FALSE(first->BuildConfigurationSchema().Value().FindDescriptor(SettingKey{"alpha.count"}));
    CHECK(first->ConfigurationEnvironmentBindings().empty());
    CHECK(std::get<std::int64_t>(captured.Get(SettingKey{"beta.count"})) == 7);
}

TEST_CASE("Composition registers and activates modules in validated order", "[unit][foundation][modules][composition]") {
    ResetLog();
    ModuleHost host;

    ModuleDescriptor editor = MakeLoggedModule("horo.editor");
    editor.dependencies.push_back(ModuleDependency{.module = ModuleId{"horo.foundation"}});

    REQUIRE(host.Register(editor).HasValue());
    REQUIRE(host.Register(MakeLoggedModule("horo.foundation")).HasValue());
    REQUIRE(host.StateOf(ModuleId{"horo.editor"}) == ModuleLifecycleState::Registered);

    // Registration is inert: no callback runs before ActivateRegistered.
    REQUIRE(g_log.activated.empty());

    struct TestApprovedBindings final : IDependencyBindings {
        int value{42};
    };

    const TestApprovedBindings approvedBindings{};
    const Result<std::size_t> activated = host.ActivateRegistered(&approvedBindings);
    REQUIRE(activated.HasValue());
    REQUIRE(activated.Value() == 2);
    REQUIRE(host.HasActiveModules());
    REQUIRE(host.StateOf(ModuleId{"horo.foundation"}) == ModuleLifecycleState::Active);
    REQUIRE(host.StateOf(ModuleId{"horo.editor"}) == ModuleLifecycleState::Active);
    REQUIRE(g_log.activated == std::vector<std::string>{"horo.foundation", "horo.editor"});
    REQUIRE(g_log.bindings == std::vector<ModuleActivationContext::DependencyBindings>{&approvedBindings, &approvedBindings});
}

TEST_CASE("Failed composition rolls back active modules in reverse order", "[unit][foundation][modules][composition]") {
    ResetLog();
    g_failModule = "horo.failing";

    ModuleHost host;
    ModuleDescriptor failing = MakeLoggedModule("horo.failing");
    failing.dependencies.push_back(ModuleDependency{.module = ModuleId{"horo.second"}});
    ModuleDescriptor second = MakeLoggedModule("horo.second");
    second.dependencies.push_back(ModuleDependency{.module = ModuleId{"horo.first"}});

    REQUIRE(host.Register(MakeLoggedModule("horo.first")).HasValue());
    REQUIRE(host.Register(second).HasValue());
    REQUIRE(host.Register(failing).HasValue());

    const Result<std::size_t> activated = host.ActivateRegistered(nullptr);
    REQUIRE(activated.HasError());
    // The failed module never joined the active set; earlier modules were
    // deactivated in reverse activation order, leaving nothing partially active.
    REQUIRE(g_log.activated == std::vector<std::string>{"horo.first", "horo.second", "horo.failing"});
    REQUIRE(g_log.deactivated == std::vector<std::string>{"horo.second", "horo.first"});
    REQUIRE(g_log.drained == std::vector<std::string>{"horo.second", "horo.first"});
    REQUIRE(host.StateOf(ModuleId{"horo.first"}) == ModuleLifecycleState::Stopped);
    REQUIRE(host.StateOf(ModuleId{"horo.second"}) == ModuleLifecycleState::Stopped);
    REQUIRE(host.StateOf(ModuleId{"horo.failing"}) == ModuleLifecycleState::Failed);
    REQUIRE_FALSE(host.HasActiveModules());
}

TEST_CASE("Rejected composition leaves registrations untouched for retry", "[unit][foundation][modules][composition]") {
    ResetLog();
    ModuleHost host;
    ModuleDescriptor missingDependency = MakeModule("horo.consumer");
    missingDependency.dependencies.push_back(ModuleDependency{.module = ModuleId{"horo.absent"}});
    REQUIRE(host.Register(missingDependency).HasValue());

    // Graph validation fails before any callback: no module was activated.
    REQUIRE(host.ActivateRegistered(nullptr).HasError());
    REQUIRE(g_log.activated.empty());
    REQUIRE(host.StateOf(ModuleId{"horo.consumer"}) == ModuleLifecycleState::Registered);
    REQUIRE_FALSE(host.HasActiveModules());

    ModuleDescriptor provider = MakeModule("horo.absent");
    REQUIRE(host.Register(provider).HasValue());
    const Result<std::size_t> retried = host.ActivateRegistered(nullptr);
    REQUIRE(retried.HasValue());
    REQUIRE(retried.Value() == 2);
}

TEST_CASE("Headless composition never activates GUI-only modules", "[unit][foundation][modules][composition]") {
    ResetLog();
    ModuleHost host;

    // A GUI adapter requires a window surface capability no headless module provides.
    ModuleDescriptor renderNull = MakeModule("horo.render.null");
    renderNull.providedCapabilities.push_back(ModuleCapabilityId{"horo.render.device"});
    ModuleDescriptor gui = MakeModule("horo.gui");
    gui.requiredCapabilities.push_back(ModuleCapabilityId{"horo.window.surface"});
    gui.lifecycle = ModuleLifecycleCallbacks{.activate = &LogActivate, .drain = &LogDrain, .deactivate = &LogDeactivate};

    REQUIRE(host.Register(renderNull).HasValue());
    REQUIRE(host.Register(gui).HasValue());
    REQUIRE(host.ActivateRegistered(nullptr).HasError());
    REQUIRE(g_log.activated.empty());

    // The headless composition root simply never registers the GUI descriptor.
    ModuleHost headless;
    REQUIRE(headless.Register(renderNull).HasValue());
    const Result<std::size_t> activated = headless.ActivateRegistered(nullptr);
    REQUIRE(activated.HasValue());
    REQUIRE(activated.Value() == 1);
}

TEST_CASE("Registration rejects duplicates and repeated dependencies", "[unit][foundation][modules][composition]") {
    ResetLog();
    ModuleHost host;
    REQUIRE(host.Register(MakeModule("horo.a")).HasValue());
    REQUIRE(host.Register(MakeModule("horo.a")).HasError());

    ModuleDescriptor repeated = MakeModule("horo.b");
    repeated.dependencies.push_back(ModuleDependency{.module = ModuleId{"horo.a"}});
    repeated.dependencies.push_back(ModuleDependency{.module = ModuleId{"horo.a"}});
    REQUIRE(host.Register(repeated).HasError());
}

TEST_CASE("Incremental activation rolls back only modules started by the failed call", "[unit][foundation][modules][composition]") {
    ResetLog();
    g_failModule = "horo.late_failure";

    ModuleHost host;
    REQUIRE(host.Register(MakeLoggedModule("horo.early")).HasValue());
    const Result<std::size_t> first = host.ActivateRegistered(nullptr);
    REQUIRE(first.HasValue());
    REQUIRE(first.Value() == 1);

    // Compose-then-activate again: Register explicitly permits IDs that are
    // not already active, so this flow is supported.
    ModuleDescriptor lateFailure = MakeLoggedModule("horo.late_failure");
    lateFailure.dependencies.push_back(ModuleDependency{.module = ModuleId{"horo.late_started"}});
    ModuleDescriptor lateStarted = MakeLoggedModule("horo.late_started");
    REQUIRE(host.Register(lateStarted).HasValue());
    REQUIRE(host.Register(lateFailure).HasValue());

    g_log.deactivated.clear();
    g_log.activated.clear();
    const Result<std::size_t> second = host.ActivateRegistered(nullptr);
    REQUIRE(second.HasError());

    // The module started in this pass is rolled back, while the earlier successful
    // activation survives untouched and stays active.
    REQUIRE(g_log.deactivated == std::vector<std::string>{"horo.late_started"});
    REQUIRE(g_log.drained == std::vector<std::string>{"horo.late_started"});
    REQUIRE(g_log.activated == std::vector<std::string>{"horo.late_started", "horo.late_failure"});
    REQUIRE(g_activationCompletions == std::vector<std::string>{"horo.early", "horo.late_started"});
    REQUIRE(host.HasActiveModules());
    REQUIRE(g_instancesAlive == 1);
    REQUIRE(host.StateOf(ModuleId{"horo.early"}) == ModuleLifecycleState::Active);
    REQUIRE(host.StateOf(ModuleId{"horo.late_started"}) == ModuleLifecycleState::Stopped);
    REQUIRE(host.StateOf(ModuleId{"horo.late_failure"}) == ModuleLifecycleState::Failed);

    host.DeactivateAll();
    REQUIRE(g_log.deactivated == std::vector<std::string>{"horo.late_started", "horo.early"});
    REQUIRE_FALSE(host.HasActiveModules());
    REQUIRE(g_instancesAlive == 0);
}

TEST_CASE("Deactivation releases attached instances exactly once", "[unit][foundation][modules][composition]") {
    ResetLog();
    ModuleHost host;
    REQUIRE(host.Register(MakeLoggedModule("horo.owner")).HasValue());
    REQUIRE(host.ActivateRegistered(nullptr).HasValue());
    REQUIRE(g_instancesAlive > 0);

    host.DeactivateAll();
    REQUIRE(g_log.deactivated == std::vector<std::string>{"horo.owner"});
    REQUIRE(g_instancesAlive == 0);
    REQUIRE_FALSE(host.HasActiveModules());

    // Idempotent teardown.
    host.DeactivateAll();
    REQUIRE(g_instancesAlive == 0);

    // Pending registrations are part of host teardown as documented.
    REQUIRE(host.Register(MakeModule("horo.pending")).HasValue());
    host.DeactivateAll();
    REQUIRE(host.StateOf(ModuleId{"horo.pending"}) == ModuleLifecycleState::Stopped);
    REQUIRE(host.Register(MakeModule("horo.pending")).HasError());
}
