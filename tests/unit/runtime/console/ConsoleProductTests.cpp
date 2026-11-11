#include "Horo/Debug/ConsoleProduct.h"
#include "Horo/Debug/ConsoleVariableSet.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Debug;

TEST_CASE("Console default catalog is compiled for exactly one product artifact", "[console][profile]") {
    const auto descriptors = ProductConsoleDescriptors();
    const auto profile = CompiledConsoleProfile();
    const auto present = [&](DebugCommandId id) {
        return std::ranges::find(descriptors, id, &DebugCommandDescriptor::id) != descriptors.end();
    };
    CHECK(present(HelpCommandId));
    CHECK(present(VersionCommandId));
    CHECK(present(ScreenshotCommandId) == (profile != ProductProfile::Server));
    const bool development = profile == ProductProfile::Editor || profile == ProductProfile::Development;
    CHECK(present(SceneTreeCommandId) == development);
    CHECK(present(InspectCommandId) == development);
    CHECK(present(SupportBundleCommandId) == (development || profile == ProductProfile::Diagnostics));
    CHECK(present(LogLevelCommandId) == (profile != ProductProfile::Shipping));
    CHECK(present(MetricsCommandId) == (profile != ProductProfile::Shipping));
    CHECK(present(ProfileCaptureCommandId) == (profile != ProductProfile::Shipping));
    RegistryPolicy policy;
    policy.profile = profile;
    policy.shippingAllowlist = {HelpCommandId, VersionCommandId};
    if (profile != ProductProfile::Server)
        policy.shippingAllowlist.push_back(ScreenshotCommandId);
    auto registry = CreateProductConsoleRegistry(policy);
    REQUIRE(registry.HasValue());
    RegistryPolicy mismatched = policy;
    mismatched.profile = profile == ProductProfile::Shipping ? ProductProfile::Development : ProductProfile::Shipping;
    CHECK(CreateProductConsoleRegistry(mismatched).HasError());
    if (profile == ProductProfile::Shipping) {
        CHECK(registry.Value().Discover({}, {15, true, true}).size() == 3);
        CHECK(registry.Value().Prepare("game.inspect 1", {15, true, true}).HasError());
        CHECK(registry.Value().Prepare("diag.support_bundle", {15, true, true}).HasError());
    } else {
        CHECK(registry.Value().Prepare("sys.profile_capture start --seconds 0", {3}).HasError());
        CHECK(registry.Value().Prepare("sys.profile_capture start --seconds 121", {3}).HasError());
        CHECK(registry.Value().Prepare("sys.profile_capture start --seconds 20", {3}).HasValue());
        CHECK(registry.Value().Prepare("log.level renderer arbitrary", {3}).HasError());
    }
    if (present(SupportBundleCommandId)) {
        CHECK(registry.Value().Prepare("diag.support_bundle", {15, true, false}).HasError());
        CHECK(registry.Value().Prepare("diag.support_bundle", {15, true, true}).HasValue());
    }
}

TEST_CASE("Console variable catalog follows the compiled profile and separate shipping variable allowlist", "[console][profile]") {
    const auto variables = ProductConsoleVariableDescriptors();
    RegistryPolicy policy;
    policy.profile = CompiledConsoleProfile();
    if (policy.profile != ProductProfile::Server)
        policy.shippingVariableAllowlist = {ConsoleVisibleVariableId};
    const auto commands = ProductConsoleDescriptors();
    auto created = ConsoleVariableSet::Create(variables, commands, policy);
    REQUIRE(created.HasValue());
    const auto allowed = created.Value().Discover({}, {15, true, true});
    if (policy.profile == ProductProfile::Shipping) {
        REQUIRE(allowed.size() == 1);
        CHECK(allowed.front().id == ConsoleVisibleVariableId);
        CHECK(created.Value().Read("console.history.limit", {15, true, true}).HasError());
    } else {
        auto changed = created.Value().Set("console.history.limit", std::int64_t{1}, {3});
        CHECK(changed.HasValue());
        CHECK(created.Value().Set("console.history.limit", std::int64_t{0}, {3}).HasError());
        REQUIRE(created.Value().Apply(ConsoleVariableApplyPolicy::NextFrame).HasValue());
        CHECK(std::get<std::int64_t>(created.Value().Read("console.history.limit", {3}).Value().active) == 1);
    }
    if (policy.profile == ProductProfile::Server) {
        CHECK(created.Value().Read("console.visible", {15, true, true}).HasError());
        CHECK(created.Value().Discover("overlay.", {15, true, true}, true).empty());
    }
}

TEST_CASE("Console composition requires real handlers for every admitted default capability", "[console][profile]") {
    RegistryPolicy policy;
    policy.profile = CompiledConsoleProfile();
    if (policy.profile == ProductProfile::Shipping)
        policy.shippingAllowlist = {HelpCommandId, ScreenshotCommandId};
    auto created = CommandRegistry::Create(ProductConsoleDescriptors(), policy);
    REQUIRE(created.HasValue());
    auto registry = std::make_shared<CommandRegistry>(std::move(created).Value());
    auto bindings = BindCoreConsoleCommands(registry, std::make_shared<ConsoleHistory>(), "test");
    REQUIRE(bindings.HasValue());
    JobSystem jobs(JobSystemConfig{.workerCount = 0});
    auto executor = CommandExecutor::Create(registry, bindings.Value(), jobs, std::make_shared<OperationStore>(8, 8),
                                            std::this_thread::get_id(), std::this_thread::get_id());
    REQUIRE(executor.HasError());
    CHECK(executor.ErrorValue().code.Value() == "console.missing_capability");
}
