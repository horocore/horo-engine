#include "Horo/Debug/ConsoleStartup.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Debug;

namespace {
    ConfigurationSchema Schema(std::span<const SettingDescriptor> descriptors) {
        ConfigurationSchema schema;
        for (const auto &descriptor : descriptors)
            REQUIRE(schema.Register(descriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        return schema;
    }

    std::vector<SettingDescriptor> Descriptors(ProductProfile profile = ProductProfile::Development) {
        auto descriptors = ConsoleConfigurationDescriptors(profile);
        REQUIRE(descriptors.HasValue());
        return std::move(descriptors).Value();
    }
}  // namespace

TEST_CASE("Console launch uses the canonical Invocation resolver and retains an immutable captured revision", "[console][configuration]") {
    const std::array<std::string_view, 8> argv{"--console",        "--dev-overlay=jobs",   "--exec", "version",
                                               "--console-script", "scripts/boot.console", "--exec", "help"};
    auto captured = ConsoleLaunchArguments::Parse(argv);
    REQUIRE(captured.HasValue());
    ConfigurationService configuration(Schema(Descriptors()));
    const auto initial = configuration.Snapshot();
    CHECK(ResolveConsoleLaunch(captured.Value(), initial, ProductProfile::Development).HasError());
    ConfigurationResolutionRequest sources;
    sources.project.emplace(SettingKey(std::string(ConsoleEnabledSettingName)), ConfigurationInputValue{false, {}});
    sources.user.emplace(SettingKey(std::string(ConsoleHistorySettingName)), ConfigurationInputValue{std::int64_t{1024}, {}});
    sources.user.emplace(*ConsoleOverlayVisibilityKey(OverlaySource::Network), ConfigurationInputValue{true, {}});
    sources.invocation = captured.Value().ConfigurationInputs();
    REQUIRE(configuration.ResolveAndCommit(sources).HasValue());
    const auto frozen = configuration.Snapshot();
    auto launch = ResolveConsoleLaunch(captured.Value(), frozen, ProductProfile::Development);
    REQUIRE(launch.HasValue());
    CHECK(launch.Value().console);
    CHECK(launch.Value().overlays);
    CHECK(launch.Value().historyLimit == 1024);
    CHECK(launch.Value().configurationRevision == frozen.Revision());
    REQUIRE(launch.Value().panels.size() == 1);
    CHECK(launch.Value().panels[0] == OverlaySource::Jobs);
    REQUIRE(launch.Value().startup.size() == 3);
    CHECK(launch.Value().startup[0].kind == ConsoleStartupKind::Command);
    CHECK(launch.Value().startup[0].text == "version");
    CHECK(launch.Value().startup[1].kind == ConsoleStartupKind::Script);
    CHECK(launch.Value().startup[1].text == "scripts/boot.console");
    CHECK(launch.Value().startup[2].kind == ConsoleStartupKind::Command);
    CHECK(launch.Value().startup[2].text == "help");
    const auto *winning = frozen.FindResolved(SettingKey(std::string(ConsoleEnabledSettingName)));
    REQUIRE(winning);
    CHECK(winning->source == ConfigurationSource::Invocation);
    sources.user[SettingKey(std::string(ConsoleHistorySettingName))] = {std::int64_t{2048}, {}};
    REQUIRE(configuration.ResolveAndCommit(sources).HasValue());
    CHECK(ReadConsoleConfiguration(configuration.Snapshot()).Value().historyLimit == 2048);
    CHECK(ReadConsoleConfiguration(frozen).Value().historyLimit == 1024);
    CHECK(ResolveConsoleLaunch(captured.Value(), frozen, ProductProfile::Development).Value().historyLimit == 1024);
}

TEST_CASE("Console startup authorization cannot be expanded by higher precedence preference sources", "[console][configuration][policy]") {
    ConfigurationService configuration(Schema(Descriptors()));
    const std::array<std::string_view, 2> argv{"--exec", "version"};
    auto captured = ConsoleLaunchArguments::Parse(argv);
    REQUIRE(captured.HasValue());
    ConfigurationResolutionRequest sources;
    const SettingKey permission{std::string(ConsoleStartupSettingName)};
    sources.project.emplace(permission, ConfigurationInputValue{false, {}});
    REQUIRE(configuration.ResolveAndCommit(sources).HasValue());
    const auto before = configuration.Snapshot();
    CHECK(ResolveConsoleLaunch(captured.Value(), before, ProductProfile::Development).HasError());
    for (auto member : {&ConfigurationResolutionRequest::invocation, &ConfigurationResolutionRequest::environment,
                        &ConfigurationResolutionRequest::session, &ConfigurationResolutionRequest::user}) {
        auto attempted = sources;
        (attempted.*member).emplace(permission, ConfigurationInputValue{true, {}});
        CHECK(configuration.ResolveAndCommit(attempted).HasError());
        CHECK(configuration.Snapshot().Revision() == before.Revision());
        CHECK_FALSE(ReadConsoleConfiguration(configuration.Snapshot()).Value().startupAllowed);
    }
}

TEST_CASE("Console settings reject missing malformed and secret schemas without ambient defaults", "[console][configuration]") {
    ConfigurationService missing(Schema({}));
    CHECK(ReadConsoleConfiguration(missing.Snapshot()).HasError());
    CHECK(ReadConsoleHistoryLimit(missing.Snapshot()).HasError());
    CHECK(ConsoleConfigurationDescriptors(static_cast<ProductProfile>(99)).HasError());
    for (const auto value : {std::int64_t{-1}, std::int64_t{0}, std::int64_t{10001}}) {
        auto descriptors = Descriptors();
        for (auto &descriptor : descriptors)
            if (descriptor.key.Value() == ConsoleHistorySettingName)
                descriptor.defaultValue = value;
        ConfigurationService invalid(Schema(descriptors));
        CHECK(ReadConsoleConfiguration(invalid.Snapshot()).HasError());
        CHECK(ReadConsoleHistoryLimit(invalid.Snapshot()).HasError());
    }
    auto descriptors = Descriptors();
    for (auto &descriptor : descriptors)
        if (descriptor.key.Value() == ConsoleHistorySettingName) {
            descriptor.type = SettingValueType::Boolean;
            descriptor.defaultValue = true;
        }
    ConfigurationService wrongType(Schema(descriptors));
    CHECK(ReadConsoleConfiguration(wrongType.Snapshot()).HasError());
    CHECK(ReadConsoleHistoryLimit(wrongType.Snapshot()).HasError());
    descriptors = Descriptors();
    for (auto &descriptor : descriptors)
        if (descriptor.key.Value() == ConsoleEnabledSettingName) {
            descriptor.type = SettingValueType::String;
            descriptor.defaultValue = std::string("opaque-private-binding");
            descriptor.sensitivity = SettingSensitivity::SecretReference;
        }
    ConfigurationService secret(Schema(descriptors));
    CHECK(ReadConsoleConfiguration(secret.Snapshot()).HasError());
}

TEST_CASE("History preference projection reads only its canonical public setting and retains captured revisions",
          "[console][configuration]") {
    auto descriptors = Descriptors();
    const SettingKey key{std::string(ConsoleHistorySettingName)};
    std::erase_if(descriptors, [&](const auto &descriptor) {
        return descriptor.key != key;
    });
    REQUIRE(descriptors.size() == 1);
    ConfigurationService configuration(Schema(descriptors));
    const auto original = configuration.Snapshot();
    CHECK(ReadConsoleHistoryLimit(original).Value() == 500);
    ConfigurationResolutionRequest sources;
    sources.session.emplace(key, ConfigurationInputValue{std::int64_t{1}, {}});
    REQUIRE(configuration.ResolveAndCommit(sources).HasValue());
    CHECK(ReadConsoleHistoryLimit(configuration.Snapshot()).Value() == 1);
    CHECK(ReadConsoleHistoryLimit(original).Value() == 500);
    descriptors[0].type = SettingValueType::String;
    descriptors[0].defaultValue = std::string("private-binding");
    descriptors[0].sensitivity = SettingSensitivity::SecretReference;
    ConfigurationService secret(Schema(descriptors));
    CHECK(ReadConsoleHistoryLimit(secret.Snapshot()).HasError());
}

TEST_CASE("The shared history variable contribution cannot be admitted by a Shipping variable allowlist",
          "[console][configuration][profile]") {
    auto owner = std::make_shared<ConfigurationService>(Schema(Descriptors(ProductProfile::Shipping)));
    RegistryPolicy policy;
    policy.profile = ProductProfile::Shipping;
    policy.shippingVariableAllowlist = {ConsoleHistoryLimitVariableId};
    auto variable = ConsoleVariable::Create(ConsoleHistoryVariableDescriptor(), policy, owner);
    REQUIRE(variable.HasError());
    CHECK(variable.ErrorValue().code.Value() == "console.permission_denied");
    CHECK(ReadConsoleHistoryLimit(owner->Snapshot()).Value() == 500);
}

TEST_CASE("Console panel requests use one typed inventory and do not carry Scene visibility across profiles",
          "[console][configuration][profile]") {
    CHECK(ParseOverlaySource("jobs") == OverlaySource::Jobs);
    CHECK(ParseOverlaySource("scene") == OverlaySource::Scene);
    CHECK_FALSE(ParseOverlaySource("Scene"));
    CHECK_FALSE(ParseOverlaySource("unknown"));
    CHECK(OverlaySourceName(OverlaySource::Count).empty());
    CHECK_FALSE(ConsoleOverlayVisibilityKey(OverlaySource::Count));
    const std::array<std::string_view, 1> argv{"--dev-overlay=scene"};
    auto captured = ConsoleLaunchArguments::Parse(argv);
    REQUIRE(captured.HasValue());
    for (const auto profile : {ProductProfile::Editor, ProductProfile::Development, ProductProfile::Profile, ProductProfile::Shipping,
                               ProductProfile::Diagnostics, ProductProfile::Server}) {
        ConfigurationService configuration(Schema(Descriptors(profile)));
        ConfigurationResolutionRequest sources;
        sources.invocation = captured.Value().ConfigurationInputs();
        REQUIRE(configuration.ResolveAndCommit(sources).HasValue());
        const bool development = profile == ProductProfile::Editor || profile == ProductProfile::Development;
        CHECK(ResolveConsoleLaunch(captured.Value(), configuration.Snapshot(), profile).HasValue() == development);
    }
    const std::array<std::string_view, 2> all{"--dev-overlays", "--dev-overlay=network"};
    auto defaults = ConsoleLaunchArguments::Parse(all);
    REQUIRE(defaults.HasValue());
    ConfigurationService configuration(Schema(Descriptors()));
    ConfigurationResolutionRequest sources;
    sources.invocation = defaults.Value().ConfigurationInputs();
    REQUIRE(configuration.ResolveAndCommit(sources).HasValue());
    auto launch = ResolveConsoleLaunch(defaults.Value(), configuration.Snapshot(), ProductProfile::Development);
    REQUIRE(launch.HasValue());
    CHECK(launch.Value().panels.size() == 5);
    CHECK(ResolveConsoleLaunch(defaults.Value(), configuration.Snapshot(), ProductProfile::Shipping, true).HasError());
}
