#include "Horo/Cli/CliErrors.h"
#include "PrefabCliHost.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <utility>

namespace Horo::Application::Internal {
    TEST_CASE("Prefab cook cannot acquire write authority from a configured project root", "[prefab-cli][authorization]") {
        using namespace Cli;
        const auto descriptor = DescribePrefabCookCommand();
        const auto &grants = descriptor.requiredCapabilities;
        auto registry = CliCommandRegistry::Create(std::array{descriptor}, {.activeHost = CliHostKind::HoroEngine,
                                                                            .supportedContractVersion = {1, 0, 0},
                                                                            .grantedCapabilities = grants});
        REQUIRE(registry.HasValue());
        std::vector<CliCommandAdapterRegistration> registrations;
        registrations.push_back(MakePrefabCookCommandAdapter());
        auto admitted = CliDispatcher::Create(std::move(registry).Value(), std::move(registrations),
                                              {.activeHost = CliHostKind::HoroEngine,
                                               .maximumSideEffects = CliSideEffectPolicy::WritesFiles,
                                               .grantedCapabilities = grants});
        REQUIRE(admitted.HasValue());
        ConfigurationSchema schema;
        REQUIRE(schema.Seal().HasValue());
        const auto configuration = ConfigurationResolver::Resolve(schema, {}, 1);
        REQUIRE(configuration.HasValue());
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path =
            (std::filesystem::temp_directory_path() / ("horo-unadmitted-prefab-" + std::to_string(suffix))).generic_u8string();
        const CliCommandRequest request{.command = descriptor.path,
                                        .options = {{.name = "project",
                                                     .values = {{.value = CliPathValue{std::string{path.begin(), path.end()}},
                                                                 .source = CliValueSource::Configuration}}},
                                                    {.name = "target",
                                                     .values = {
                                                         {.value = std::string{"headless-null"}, .source = CliValueSource::Invocation}}}}};
        auto dispatcher = std::move(admitted).Value();
        const auto outcome = dispatcher.Dispatch(request, {.invocation = {1}, .configuration = &configuration.Value()});
        REQUIRE(outcome.Outcome().HasError());
        CHECK(outcome.Outcome().ErrorValue().code.Value() == CliErrors::ParseFailed.code.Value());
        CHECK_FALSE(outcome.Correlation().operation.has_value());
    }
}  // namespace Horo::Application::Internal
