#include "Horo/Debug/ConsoleStartup.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Debug;

namespace {
    Result<ConsoleLaunchConfiguration> Launch(std::span<const std::string_view> arguments, ProductProfile profile,
                                              bool shippingUi = false) {
        auto captured = ConsoleLaunchArguments::Parse(arguments);
        if (captured.HasError())
            return Result<ConsoleLaunchConfiguration>::Failure(captured.ErrorValue());
        auto descriptors = ConsoleConfigurationDescriptors(profile);
        REQUIRE(descriptors.HasValue());
        ConfigurationSchema schema;
        for (const auto &descriptor : descriptors.Value())
            REQUIRE(schema.Register(descriptor).HasValue());
        REQUIRE(schema.Seal().HasValue());
        ConfigurationService configuration(std::move(schema));
        ConfigurationResolutionRequest sources;
        sources.invocation = captured.Value().ConfigurationInputs();
        REQUIRE(configuration.ResolveAndCommit(sources).HasValue());
        return ResolveConsoleLaunch(captured.Value(), configuration.Snapshot(), profile, shippingUi);
    }

    class ScriptFiles final : public Platform::IProjectReadFiles {
    public:
        mutable int reads{};
        std::string script{"version\r\n\nversion\n"};

        Result<Platform::ProjectReadManifest> Files(std::string_view, const Platform::ProjectReadContext &,
                                                    const Platform::ProjectReadLimits &) const override {
            return Result<Platform::ProjectReadManifest>::Failure(MakeCommandError(CommandError::MissingCapability));
        }

        Result<Platform::ProjectReadTextSnapshot> Text(std::string_view path, const Platform::ProjectReadContext &context,
                                                       const Platform::ProjectReadLimits &) const override {
            ++reads;
            return Result<Platform::ProjectReadTextSnapshot>::Success({context.projectIdentity, context.projectGeneration,
                                                                       std::string(path), "revision",
                                                                       std::make_shared<const std::string>(script)});
        }
    };
}  // namespace

TEST_CASE("Console launch parsing enforces surfaces across artifact profiles", "[console]") {
    const std::array<std::string_view, 0> empty{};
    const std::array<std::string_view, 1> console{"--console"};
    const std::array<std::string_view, 1> overlays{"--dev-overlay=jobs"};
    const std::array<std::string_view, 1> remote{"--remote-console=localhost:9000"};
    for (auto profile : {ProductProfile::Editor, ProductProfile::Development, ProductProfile::Profile, ProductProfile::Shipping,
                         ProductProfile::Diagnostics, ProductProfile::Server}) {
        auto defaults = Launch(empty, profile);
        REQUIRE(defaults.HasValue());
        CHECK(defaults.Value().console == (profile == ProductProfile::Development));
        CHECK(Launch(console, profile).HasValue() == (profile != ProductProfile::Shipping && profile != ProductProfile::Server));
        CHECK(Launch(overlays, profile).HasValue() == (profile != ProductProfile::Shipping && profile != ProductProfile::Server));
        CHECK(Launch(remote, profile).HasError());
    }
    CHECK(Launch(console, ProductProfile::Shipping, true).HasValue());
    const std::array<std::string_view, 1> unknown{"--arbitrary"};
    CHECK(Launch(unknown, ProductProfile::Development).HasError());
    const std::array<std::string_view, 1> missing{"--exec"};
    CHECK(Launch(missing, ProductProfile::Development).HasError());
    const std::array<std::string_view, 2> unsafe{"--console-script", "../escape"};
    CHECK(Launch(unsafe, ProductProfile::Development).HasError());
    const std::array<std::string_view, 2> duplicate{"--dev-overlay=logs", "--dev-overlay=logs"};
    CHECK(Launch(duplicate, ProductProfile::Development).HasError());
}

TEST_CASE("Console scripts validate complete rooted allowlisted captures before dispatch", "[console]") {
    RegistryPolicy policy;
    policy.startupAllowlist = {1};
    const std::array commands{DebugCommandDescriptor{1,
                                                     "version",
                                                     "Version",
                                                     "version",
                                                     {},
                                                     CommandPermission::Public,
                                                     CommandThreadPolicy::ImmediateConsoleThread,
                                                     CommandAvailability::ShippingAllowlist}};
    auto result = CommandRegistry::Create(commands, policy);
    REQUIRE(result.HasValue());
    ScriptFiles files;
    const std::array<std::string_view, 1> allowed{"scripts/startup.console"};
    Platform::ProjectReadContext context{"project", 1, {}, std::chrono::steady_clock::now() + std::chrono::seconds(5), {}};
    auto script = ReadConsoleScript(allowed[0], allowed, files, context, result.Value(), {});
    REQUIRE(script.HasValue());
    CHECK(script.Value().size() == 2);
    CHECK(files.reads == 1);
    for (const auto path : {"../escape", "scripts/startup.console.bak", "C:/secret", "scripts/other.console"})
        CHECK(ReadConsoleScript(path, allowed, files, context, result.Value(), {}).HasError());
    CHECK(files.reads == 1);
    for (const auto text : {"version\nversion extra", "version\n\"unterminated", "version | version"}) {
        files.script = text;
        CHECK(ReadConsoleScript(allowed[0], allowed, files, context, result.Value(), {}).HasError());
    }
    files.script.assign(65537, 'a');
    CHECK(ReadConsoleScript(allowed[0], allowed, files, context, result.Value(), {}).HasError());
}
