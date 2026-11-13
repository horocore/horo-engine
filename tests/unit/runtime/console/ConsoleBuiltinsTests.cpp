#include "Horo/Debug/ConsoleBuiltins.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Debug;

TEST_CASE("Core console commands share policy discovery and only clear the visible owner history", "[console]") {
    std::vector<DebugCommandDescriptor> descriptors(CoreConsoleDescriptors().begin(), CoreConsoleDescriptors().end());
    descriptors.push_back({42,
                           "game.secret",
                           "Hidden command",
                           "game.secret",
                           {},
                           CommandPermission::Restricted,
                           CommandThreadPolicy::OwnerThreadNextFrame,
                           CommandAvailability::DiagnosticsOnly});
    RegistryPolicy policy;
    auto created = CommandRegistry::Create(descriptors, policy);
    REQUIRE(created.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(created).Value());
    auto history = std::make_shared<ConsoleHistory>();
    const auto prepared = registry->Prepare("version", {});
    REQUIRE(prepared.HasValue());
    history->Append(prepared.Value(), "old");
    auto bound = BindCoreConsoleCommands(registry, history, "engine/game Debug Windows Development");
    REQUIRE(bound.HasValue());
    REQUIRE(bound.Value().size() == 4);
    for (const auto &binding : bound.Value()) {
        if (binding.id == HelpCommandId || binding.id == FindCommandId) {
            const std::array<ArgumentValue, 1> query{std::string("")};
            auto output = binding.handler->Execute({}, query, {}, nullptr);
            REQUIRE(output.HasValue());
            CHECK(output.Value().text.find("game.secret") == std::string::npos);
            CHECK(output.Value().text.find("version") != std::string::npos);
        }
        if (binding.id == VersionCommandId) {
            auto output = binding.handler->Execute({}, {}, {}, nullptr);
            REQUIRE(output.HasValue());
            CHECK(output.Value().text == "engine/game Debug Windows Development");
        }
        if (binding.id == ClearCommandId) {
            REQUIRE(binding.handler->Execute({}, {}, {}, nullptr).HasValue());
            CHECK(history->Entries().empty());
        }
    }
}

TEST_CASE("Shipping core command binding inventory contains exactly approved entries", "[console]") {
    RegistryPolicy policy;
    policy.profile = ProductProfile::Shipping;
    policy.shippingAllowlist = {HelpCommandId, VersionCommandId};
    auto created = CommandRegistry::Create(CoreConsoleDescriptors(), policy);
    REQUIRE(created.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(created).Value());
    auto bindings = BindCoreConsoleCommands(registry, std::make_shared<ConsoleHistory>(), "Shipping");
    REQUIRE(bindings.HasValue());
    REQUIRE(bindings.Value().size() == 2);
    CHECK(registry->Prepare("clear", {}).HasError());
    CHECK(registry->Prepare("find private", {}).HasError());
}
