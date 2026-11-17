#include "Horo/Debug/CommandRegistry.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <utility>

using namespace Horo::Debug;

namespace {
    DebugCommandDescriptor Command(DebugCommandId id, std::string_view name) {
        return {id,
                name,
                "Test command",
                name,
                {},
                CommandPermission::Public,
                CommandThreadPolicy::ImmediateConsoleThread,
                CommandAvailability::ShippingAllowlist};
    }

    CommandRegistry Registry(std::span<const DebugCommandDescriptor> commands, RegistryPolicy policy = {}) {
        auto result = CommandRegistry::Create(commands, policy);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }
}  // namespace

TEST_CASE("Console composition rejects collisions and owns all metadata", "[console]") {
    std::string name = "game.inspect";
    std::string option = "entity";
    std::string enumValue = "player";
    std::array<std::string_view, 1> enumeration{enumValue};
    std::array<DebugArgumentDescriptor, 1> args{{{option, "Entity", DebugArgumentType::Enum, true, {}, false, false, enumeration}}};
    std::array commands{Command(1, name)};
    commands[0].arguments = args;
    auto registry = Registry(commands);
    name.assign("destroyed");
    option.assign("destroyed");
    enumValue.assign("destroyed");
    CHECK(registry.Prepare("game.inspect player", {}).HasValue());
    auto moved = std::move(registry);
    CHECK(moved.Prepare("game.inspect player", {}).HasValue());
    CHECK(registry.Prepare("game.inspect player", {}).HasError());
    CHECK(registry.Discover({}, {}).empty());

    for (auto pair : {std::array{Command(1, "help"), Command(1, "version")}, std::array{Command(1, "help"), Command(2, "help")},
                      std::array{Command(1, "game.player"), Command(2, "game.player.inspect")}}) {
        auto result = CommandRegistry::Create(pair, {});
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "console.duplicate");
    }
    for (const auto nameValue : {"", "Game.inspect", ".game", "game.", "game..inspect", "9game"}) {
        const std::array invalid{Command(1, nameValue)};
        CHECK(CommandRegistry::Create(invalid, {}).HasError());
    }
}

TEST_CASE("Console grammar converts complete typed arguments and rejects malformed input", "[console]") {
    const std::array<DebugArgumentDescriptor, 3> args{{{"text", {}, DebugArgumentType::String},
                                                       {"count", {}, DebugArgumentType::Int32},
                                                       {"enabled", {}, DebugArgumentType::Bool, false, "false", false, true}}};
    std::array commands{Command(1, "game.test")};
    commands[0].arguments = args;
    auto registry = Registry(commands);
    auto result = registry.Prepare(" game.test \"quoted \\\"value\\\"\" -42 --enabled true ", {});
    REQUIRE(result.HasValue());
    CHECK(std::get<std::string>(result.Value().arguments[0]) == "quoted \"value\"");
    CHECK(std::get<std::int64_t>(result.Value().arguments[1]) == -42);
    CHECK(std::get<bool>(result.Value().arguments[2]));
    REQUIRE(registry.Prepare("game.test '' 0", {}).HasValue());
    auto bareOption = registry.Prepare("game.test enabled 1 enabled true", {});
    REQUIRE(bareOption.HasValue());
    CHECK(std::get<std::string>(bareOption.Value().arguments[0]) == "enabled");
    CHECK(std::get<bool>(bareOption.Value().arguments[2]));
    REQUIRE(registry.Prepare("game.test --enabled true x 1", {}).HasValue());
    for (const auto input : {"game.test", "game.test x", "game.test x 2147483648", "game.test x 1x", "game.test x 1 extra",
                             "game.test \"x 1", "game.test x 1 --enabled", "game.test x 1 --enabled yes", "game.test x 1 --missing true",
                             "game.test x 1 --enabled true --enabled false", "game.test x 1 enabled true --enabled false",
                             "game.test x 1 | help", "game.test x 1;help", "game.test x 1\nhelp", "game.test x 1 \\q"}) {
        INFO(input);
        CHECK(registry.Prepare(input, {}).HasError());
    }
    CHECK(registry.Prepare(std::string(4097, 'x'), {}).HasError());
    CHECK(registry.Prepare(std::string("\xFF"), {}).HasError());
}

TEST_CASE("Console numeric schemas reject overflow nonfinite and invalid entity identities", "[console]") {
    for (const auto type : {DebugArgumentType::Int64, DebugArgumentType::Float, DebugArgumentType::EntityId}) {
        const std::array<DebugArgumentDescriptor, 1> args{{{"value", {}, type}}};
        std::array commands{Command(1, "game.test")};
        commands[0].arguments = args;
        auto registry = Registry(commands);
        for (const auto text :
             {"game.test nan", "game.test inf",
              "game.test "
              "9999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"
              "9999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999999"
              "99999999999999999999999999999999999999999999999999999999999999999999999999",
              "game.test 2junk"})
            CHECK(registry.Prepare(text, {}).HasError());
        if (type == DebugArgumentType::EntityId) {
            auto reference = registry.Prepare("game.test 7:0:2", {});
            REQUIRE(reference.HasValue());
            CHECK((std::get<DebugEntityReference>(reference.Value().arguments[0]) == DebugEntityReference{7, 0, 2}));
            for (const auto invalid : {"game.test 0", "game.test 42", "game.test 0:1:1", "game.test 1:0:0", "game.test 1:0:4294967296",
                                       "game.test 1:-1:1", "game.test 1:0:1:1"})
                CHECK(registry.Prepare(invalid, {}).HasError());
        } else
            CHECK(registry.Prepare("game.test 42", {}).HasValue());
    }
}

TEST_CASE("Console discovery and submission share principal and profile policy", "[console]") {
    std::array commands{Command(1, "help"), Command(2, "sys.inspect"), Command(3, "game.god"), Command(4, "diag.export")};
    commands[1].permissions = CommandPermission::Developer;
    commands[1].availability = CommandAvailability::NonShipping;
    commands[2].permissions = CommandPermission::AdminCheat;
    commands[2].availability = CommandAvailability::DevelopmentOnly;
    commands[3].permissions = CommandPermission::Restricted;
    commands[3].availability = CommandAvailability::DiagnosticsOnly;
    for (const auto profile : {ProductProfile::Editor, ProductProfile::Development, ProductProfile::Profile, ProductProfile::Shipping,
                               ProductProfile::Diagnostics, ProductProfile::Server}) {
        RegistryPolicy policy;
        policy.profile = profile;
        policy.shippingAllowlist = {1};
        auto registry = Registry(commands, policy);
        CHECK(registry.Discover({}, {}).size() == 1);
        CHECK(registry.Discover("he", {}, true).size() == 1);
        CHECK(registry.Prepare("help", {}).HasValue());
        CHECK(registry.Prepare("game.god", {15, false, true}).HasError());
        CHECK(registry.Prepare("diag.export", {15, true, false}).HasError());
        const auto all = registry.Discover({}, {15, true, true});
        const std::size_t expected = profile == ProductProfile::Shipping
                                         ? 1
                                         : (profile == ProductProfile::Editor || profile == ProductProfile::Development
                                                ? 4
                                                : (profile == ProductProfile::Diagnostics ? 3 : 2));
        CHECK(all.size() == expected);
    }
    commands[2].availability = CommandAvailability::ShippingAllowlist;
    CHECK(CommandRegistry::Create(commands, {}).HasError());
}

TEST_CASE("Console startup admission history privacy and retention are bounded", "[console]") {
    const std::array<DebugArgumentDescriptor, 1> args{{{"token", {}, DebugArgumentType::String, true, {}, true}}};
    std::array commands{Command(1, "game.login"), Command(2, "version")};
    commands[0].arguments = args;
    RegistryPolicy policy;
    policy.startupAllowlist = {2};
    auto registry = Registry(commands, policy);
    CHECK(registry.Prepare("game.login secret", {}, true).HasError());
    CHECK(registry.Prepare("version", {}, true).HasValue());
    auto prepared = registry.Prepare("game.login \"very secret\"", {});
    REQUIRE(prepared.HasValue());
    CHECK(prepared.Value().redactedText.find("secret") == std::string::npos);
    ConsoleLimits limits;
    limits.historyEntries = 2;
    limits.outputBytes = 4;
    ConsoleHistory history(limits);
    for (int i = 0; i < 10000; ++i)
        history.Append(prepared.Value(), "bounded output");
    REQUIRE(history.Entries().size() == 2);
    CHECK(history.Entries()[0].output == "boun");
    CHECK(history.ReverseSearch("REDACTED") != nullptr);
    CHECK(history.ReverseSearch("secret") == nullptr);
    CHECK(history.ReverseSearch("", 2) == nullptr);
    history.Clear();
    CHECK(history.Entries().empty());
}
