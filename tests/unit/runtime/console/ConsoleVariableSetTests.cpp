#include "Horo/Debug/ConsoleBuiltins.h"
#include "Horo/Debug/ConsoleVariableSet.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

using namespace Horo;
using namespace Horo::Debug;

TEST_CASE("Console variable inventory rejects duplicate ownership and exposes only allowed metadata", "[console]") {
    std::vector descriptors{ConsoleVariableDescriptor{100, "console.visible", false},
                            ConsoleVariableDescriptor{101, "console.limit", std::int64_t{2}, 1.0, 4.0}};
    auto created = ConsoleVariableSet::Create(descriptors, CoreConsoleDescriptors(), {});
    REQUIRE(created.HasValue());
    auto variables = std::move(created).Value();
    CHECK(variables.Discover({}, {}).empty());
    CHECK(variables.Discover("console.", {3}, true).size() == 2);
    REQUIRE(variables.Set("console.limit", std::int64_t{4}, {3}).HasValue());
    CHECK(std::get<std::int64_t>(variables.Read("console.limit", {3}).Value().active) == 4);
    CHECK(variables.Set("console.limit", std::int64_t{5}, {3}).HasError());
    descriptors.push_back(descriptors.front());
    CHECK(ConsoleVariableSet::Create(descriptors, CoreConsoleDescriptors(), {}).HasError());
    descriptors.pop_back();
    descriptors[0].id = HelpCommandId;
    CHECK(ConsoleVariableSet::Create(descriptors, CoreConsoleDescriptors(), {}).HasError());
}

TEST_CASE("Shipping variable inventory uses its own explicit allowlist under the same product policy", "[console]") {
    std::vector descriptors{ConsoleVariableDescriptor{100, "console.visible", false},
                            ConsoleVariableDescriptor{101, "renderer.debug.limit", std::int64_t{2}}};
    descriptors[0].permission = CommandPermission::Public;
    descriptors[0].availability = CommandAvailability::ShippingAllowlist;
    RegistryPolicy policy;
    policy.profile = ProductProfile::Shipping;
    policy.shippingVariableAllowlist = {100};
    auto created = ConsoleVariableSet::Create(descriptors, CoreConsoleDescriptors(), policy);
    REQUIRE(created.HasValue());
    CHECK(created.Value().Discover({}, {15, true, true}).size() == 1);
    CHECK(created.Value().Read("renderer.debug.limit", {15, true, true}).HasError());
    policy.shippingVariableAllowlist.push_back(101);
    CHECK(ConsoleVariableSet::Create(descriptors, CoreConsoleDescriptors(), policy).HasError());
    policy.shippingVariableAllowlist = {100};
    descriptors[1].defaultValue = std::numeric_limits<double>::quiet_NaN();
    CHECK(ConsoleVariableSet::Create(descriptors, CoreConsoleDescriptors(), policy).HasError());
}

TEST_CASE("Core console help and find share the variable inventory without leaking another profile", "[console]") {
    const std::vector descriptors{ConsoleVariableDescriptor{100, "console.limit", std::int64_t{2}}};
    auto made = ConsoleVariableSet::Create(descriptors, CoreConsoleDescriptors(), {});
    REQUIRE(made.HasValue());
    auto variables = std::make_shared<ConsoleVariableSet>(std::move(made).Value());
    auto composed = CommandRegistry::Create(CoreConsoleDescriptors(), {});
    REQUIRE(composed.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(composed).Value());
    auto bindings = BindCoreConsoleCommands(registry, std::make_shared<ConsoleHistory>(), "test", variables);
    REQUIRE(bindings.HasValue());
    for (const auto &binding : bindings.Value()) {
        if (binding.id != HelpCommandId)
            continue;
        const std::array<ArgumentValue, 1> query{std::string("")};
        auto allowed = binding.handler->Execute({3}, query, {}, nullptr);
        REQUIRE(allowed.HasValue());
        CHECK(allowed.Value().text.find("console.limit") != std::string::npos);
        auto denied = binding.handler->Execute({}, query, {}, nullptr);
        REQUIRE(denied.HasValue());
        CHECK(denied.Value().text.find("console.limit") == std::string::npos);
    }
    RegistryPolicy shipping;
    shipping.profile = ProductProfile::Shipping;
    auto shipped = CommandRegistry::Create(CoreConsoleDescriptors(), shipping);
    REQUIRE(shipped.HasValue());
    auto shippingRegistry = std::make_shared<CommandRegistry>(std::move(shipped).Value());
    CHECK(BindCoreConsoleCommands(shippingRegistry, std::make_shared<ConsoleHistory>(), "shipping", variables).HasError());
}

TEST_CASE("Console text variables use common owner dispatch with typed parsing and explicit pending state", "[console][variables]") {
    std::vector descriptors{ConsoleVariableDescriptor{100, "console.visible", false},
                            ConsoleVariableDescriptor{101, "console.limit", std::int64_t{2}, 1.0, 4.0},
                            ConsoleVariableDescriptor{102, "game.ratio", 1.0, 0.0, 2.0},
                            ConsoleVariableDescriptor{103, "game.label", std::string("initial")},
                            ConsoleVariableDescriptor{104, "game.readonly", false}};
    descriptors[1].applyPolicy = ConsoleVariableApplyPolicy::NextFrame;
    descriptors[4].applyPolicy = ConsoleVariableApplyPolicy::ReadOnly;
    auto made = ConsoleVariableSet::Create(descriptors, {}, {});
    REQUIRE(made.HasValue());
    auto variables = std::make_shared<ConsoleVariableSet>(std::move(made).Value());
    auto commands = CommandRegistry::Create({}, {});
    REQUIRE(commands.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(commands).Value());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto operations = std::make_shared<OperationStore>(8, 8);
    auto madeExecutor =
        CommandExecutor::Create(registry, {}, jobs, operations, std::this_thread::get_id(), std::this_thread::get_id(), 8, {}, variables);
    REQUIRE(madeExecutor.HasValue());
    auto &executor = *madeExecutor.Value();
    CHECK(executor.Submit("console.visible true", {}).HasError());
    CHECK(executor.Submit("console.limit 5", {3}).HasError());
    CHECK(executor.Submit("console.limit 1x", {3}).HasError());
    CHECK(executor.Submit("game.ratio nan", {3}).HasError());
    CHECK(executor.Submit("game.label words without quotes", {3}).HasError());
    CHECK(executor.Submit("game.label \"unterminated", {3}).HasError());
    CHECK(executor.Submit("game.readonly true", {3}).HasError());
    CHECK(executor.Submit("console.visible true", {3}, true).HasError());
    CHECK_FALSE(operations->SnapshotIfChanged(0));
    std::optional<Result<CommandReceipt>> submitted;
    std::thread producer([&] {
        submitted.emplace(executor.Submit("console.limit 4", {3}));
    });
    producer.join();
    REQUIRE(submitted);
    REQUIRE(submitted->HasValue());
    REQUIRE(submitted->Value().operation);
    CHECK(std::get<std::int64_t>(variables->Read("console.limit", {3}).Value().active) == 2);
    REQUIRE(executor.Submit("console.visible true", {3}).HasValue());
    REQUIRE(executor.Submit("game.ratio 1.5", {3}).HasValue());
    REQUIRE(executor.Submit("game.label \"two words\"", {3}).HasValue());
    REQUIRE(executor.DrainOwner(8).HasValue());
    const auto pending = variables->Read("console.limit", {3});
    REQUIRE(pending.HasValue());
    CHECK(std::get<std::int64_t>(pending.Value().active) == 2);
    REQUIRE(pending.Value().pending);
    CHECK(std::get<std::int64_t>(*pending.Value().pending) == 4);
    CHECK(std::get<bool>(variables->Read("console.visible", {3}).Value().active));
    CHECK(std::get<double>(variables->Read("game.ratio", {3}).Value().active) == 1.5);
    CHECK(std::get<std::string>(variables->Read("game.label", {3}).Value().active) == "two words");
    const auto snapshot = operations->SnapshotIfChanged(0);
    REQUIRE(snapshot);
    const auto found = std::ranges::find(snapshot->operations, *submitted->Value().operation, &OperationRecord::id);
    REQUIRE(found != snapshot->operations.end());
    CHECK(found->message.find("active=2 pending=4 apply=next_frame") != std::string::npos);
    REQUIRE(variables->Apply(ConsoleVariableApplyPolicy::NextFrame).HasValue());
    CHECK(std::get<std::int64_t>(variables->Read("console.limit", {3}).Value().active) == 4);
    REQUIRE(executor.Submit("game.readonly", {3}).HasValue());
    executor.Stop();
    CHECK(executor.Submit("console.visible false", {3}).HasError());
}

TEST_CASE("Console dispatcher rejects independently composed command and variable identity collisions", "[console][variables]") {
    const std::array descriptors{ConsoleVariableDescriptor{100, "game.query", false}};
    auto made = ConsoleVariableSet::Create(descriptors, {}, {});
    REQUIRE(made.HasValue());
    auto variables = std::make_shared<ConsoleVariableSet>(std::move(made).Value());
    const std::array commandDescriptors{DebugCommandDescriptor{101, "game.query", {}, {}, {}, CommandPermission::Public}};
    auto commands = CommandRegistry::Create(commandDescriptors, {});
    REQUIRE(commands.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(commands).Value());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto result = CommandExecutor::Create(registry, {}, jobs, std::make_shared<OperationStore>(2, 2), std::this_thread::get_id(),
                                          std::this_thread::get_id(), 2, {}, variables);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == "console.duplicate");
}
