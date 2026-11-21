#include "CliOutputContractFixture.h"
#include "ScriptModuleHostTestSupport.h"

#include <algorithm>
#include <thread>

namespace Horo::Extensions::Tests {
    using namespace ModuleHostSupport;

    namespace {
        std::vector<std::string> Arguments(const SignedPackage &package) {
            return {"extension",        "module-run",
                    "--runtime",        package.runtime == ScriptModuleRuntimeKind::Lua54 ? "lua" : "javascript",
                    "--extension-root", package.package.string(),
                    "--extension-id",   package.id,
                    "--module",         package.module,
                    "--script",         package.source.generic_string(),
                    "--host-trust",     package.trust.string(),
                    "--publisher",      package.key.root.publisherId,
                    "--temporary-root", package.temporary.string(),
                    "--approve-native", "--approve-script",
                    "--output",         "json"};
        }

        ExternalProcessResult Run(const SignedPackage &package, std::vector<std::string> arguments, std::string &output) {
            NativeExternalProcessRunner process;
            const auto result =
                Take(process.Run({.executable = HORO_MODULE_SCRIPT_CLI,
                                  .arguments = std::move(arguments),
                                  .workingDirectory = package.root,
                                  .environment = {.set = {{"HOME", package.root.string()}, {"USERPROFILE", package.root.string()}}},
                                  .timeout = std::chrono::seconds{30},
                                  .onOutput =
                                      [&output](ProcessOutputLine line) {
                if (line.stream == ProcessOutputStream::StandardOutput) {
                    output += line.text;
                    output += '\n';
                }
            },
                                  .maximumOutputBytes = 65536},
                                 {}));
            if (result.reason == ProcessTerminationReason::Exited && result.exitCode == 0)
                Horo::Tests::CliContract::ValidateEnvelope(nlohmann::json::parse(output));
            return result;
        }
    }  // namespace

    TEST_CASE("Actual product CLI executes equivalent signed general API source in both genuine runtimes",
              "[Extensions][ScriptModule][Cli][Native]") {
        const auto expected = Hex(Take(EncodeScriptCallResult(ScriptCallResult::Success({ScriptValue{std::int64_t{42}}}))));
        const ScriptModuleRuntimeKind vms[]{ScriptModuleRuntimeKind::Lua54, ScriptModuleRuntimeKind::JavaScriptNg};
        for (const auto vm : vms) {
            SignedPackage package{vm, SumSource(vm)};
            std::string output;
            const auto result = Run(package, Arguments(package), output);
            INFO(output);
            REQUIRE(result.exitCode == 0);
            CHECK(output.find("horo.script.call-result.v1") != std::string::npos);
            CHECK(output.find(expected) != std::string::npos);
            CHECK(std::filesystem::is_empty(package.temporary));
        }
    }

    TEST_CASE("Actual module CLI rejects denied consent and stale independent policy before native activation",
              "[Extensions][ScriptModule][Cli][Security]") {
        SignedPackage package{ScriptModuleRuntimeKind::JavaScriptNg, SumSource(ScriptModuleRuntimeKind::JavaScriptNg)};
        auto arguments = Arguments(package);
        SECTION("native consent missing") {
            const auto found = std::ranges::find(arguments, "--approve-native");
            REQUIRE(found != arguments.end());
            arguments.erase(found);
        }
        SECTION("script consent missing") {
            const auto found = std::ranges::find(arguments, "--approve-script");
            REQUIRE(found != arguments.end());
            arguments.erase(found);
        }
        SECTION("publisher revoked") {
            package.Trust(true);
        }
        SECTION("runtime source extension differs") {
            const auto found = std::ranges::find(arguments, "--runtime");
            REQUIRE(found != arguments.end());
            *(found + 1) = "lua";
        }
        std::string output;
        const auto result = Run(package, std::move(arguments), output);
        INFO(output);
        CHECK(result.exitCode != 0);
        CHECK(std::filesystem::is_empty(package.temporary));
    }

    TEST_CASE("Actual module CLI keeps the signed script under its cooperative runtime deadline",
              "[Extensions][ScriptModule][Cli][Budget]") {
        SignedPackage package{ScriptModuleRuntimeKind::JavaScriptNg, "while(true){}"};
        std::string output;
        const auto result = Run(package, Arguments(package), output);
        INFO(output);
        CHECK(result.exitCode != 0);
        CHECK(std::filesystem::is_empty(package.temporary));
    }
}  // namespace Horo::Extensions::Tests
