#include "ScriptModuleProductRecoverySupport.h"

namespace Horo::Extensions::Tests {
    using namespace ProductRecoverySupport;

    namespace {
        std::vector<std::string> Arguments(const SignedPackage &package) {
            return {"extension",
                    "module-run",
                    "--runtime",
                    package.runtime == ScriptModuleRuntimeKind::Lua54 ? "lua" : "javascript",
                    "--extension-root",
                    package.package.string(),
                    "--extension-id",
                    package.id,
                    "--module",
                    package.module,
                    "--script",
                    package.source.generic_string(),
                    "--host-trust",
                    package.trust.string(),
                    "--publisher",
                    package.key.root.publisherId,
                    "--temporary-root",
                    package.temporary.string(),
                    "--approve-native",
                    "--approve-script",
                    "--refresh-reference",
                    "--output",
                    "json"};
        }

        ExternalProcessResult Run(const SignedPackage &package, std::string &output) {
            NativeExternalProcessRunner process;
            return Take(process.Run({.executable = HORO_MODULE_SCRIPT_CLI,
                                     .arguments = Arguments(package),
                                     .workingDirectory = package.root,
                                     .environment = {.set = {{"HOME", package.root.string()}, {"USERPROFILE", package.root.string()}}},
                                     .timeout = std::chrono::seconds{30},
                                     .onOutput =
                                         [&output](ProcessOutputLine line) {
                output += line.text;
                output += '\n';
            },
                                     .maximumOutputBytes = 65536},
                                    {}));
        }
    }  // namespace

    TEST_CASE("Actual signed CLI module operations explicitly refresh the real reference provider while preserving both VM continuations",
              "[Extensions][ScriptModule][Recovery][Cli][TwoVm]") {
        const auto expected = Hex(Take(EncodeScriptCallResult(ScriptCallResult::Success({ScriptValue{std::int64_t{42}}}))));
        for (const auto vm : Vms) {
            SignedPackage package{vm, Source(vm), true, true};
            std::string output;
            const auto result = Run(package, output);
            INFO(output);
            REQUIRE(result.exitCode == 0);
            CHECK(output.find(expected) != std::string::npos);
            CHECK(std::filesystem::is_empty(package.temporary));
        }
    }

    TEST_CASE("The explicit CLI recovery option cannot grant a builtin absent from the signed source admission",
              "[Extensions][ScriptModule][Recovery][Cli][Policy]") {
        SignedPackage package{ScriptModuleRuntimeKind::Lua54, SumSource(ScriptModuleRuntimeKind::Lua54), false, true};
        std::string output;
        const auto result = Run(package, output);
        INFO(output);
        CHECK(result.exitCode != 0);
        CHECK(output.find(ExtensionErrors::ScriptInvocationUnavailable.code.Value()) != std::string::npos);
        CHECK(std::filesystem::is_empty(package.temporary));
    }
}  // namespace Horo::Extensions::Tests
