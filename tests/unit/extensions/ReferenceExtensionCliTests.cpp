#include "CliOutputContractFixture.h"
#include "ExtensionScriptCliInternal.h"
#include "Horo/Extensions/ExtensionManifest.h"
#include "Horo/Platform/ExternalProcess.h"
#include "HostSignatureDocument.h"
#include "OpenXRProductSigningTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace Horo::Extensions::Tests {
    namespace {
        std::vector<std::byte> Read(const std::filesystem::path &path) {
            std::ifstream input{path, std::ios::binary};
            REQUIRE(input);
            const std::vector<char> text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
            std::vector<std::byte> bytes;
            for (const auto character : text)
                bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
            return bytes;
        }

        void Write(const std::filesystem::path &path, const std::string_view text) {
            std::ofstream output{path, std::ios::binary | std::ios::trunc};
            REQUIRE(output);
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            REQUIRE(output);
        }

        std::string Hex(const std::span<const std::byte> bytes) {
            constexpr char alphabet[] = "0123456789abcdef";
            std::string text;
            for (const auto byte : bytes) {
                const auto value = std::to_integer<unsigned>(byte);
                text.push_back(alphabet[value >> 4U]);
                text.push_back(alphabet[value & 15U]);
            }
            return text;
        }

        std::string ErrorEvidence(const Error &error) {
            std::string output;
            const auto *current = &error;
            for (unsigned depth = 0; current && depth < 8; ++depth) {
                output += current->domain.Value() + ":" + current->code.Value() + ": " + current->message + "\n";
                current = current->cause.Get();
            }
            return output;
        }

        struct SignedPackage final {
            Horo::Tests::OpenXRProduct::SigningKey key;  // Existing canonical test signer, never a production or installed trust root.
            std::filesystem::path root, package, temporary, trust;
            std::string id;

            SignedPackage() {
                const auto *source = std::getenv("HORO_REFERENCE_PACKAGE_ROOT");
                REQUIRE(source);
                root = std::filesystem::temp_directory_path() /
                       ("horo-reference-cli-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
                REQUIRE(std::filesystem::create_directory(root));
                root = std::filesystem::canonical(root);  // Match production Path() admission, including macOS /var aliases.
                package = root / "package";
                temporary = root / "temporary";
                trust = root / "independent-trust.json";
                std::filesystem::copy(source, package, std::filesystem::copy_options::recursive);
                REQUIRE(std::filesystem::create_directory(temporary));
                const auto manifestBytes = Read(package / "extension.json");
                const auto manifest = ParseExtensionManifest({reinterpret_cast<const char *>(manifestBytes.data()), manifestBytes.size()});
                REQUIRE(manifest.HasValue());
                id = manifest.Value().id;
                Sign(package / "extension.json");
                for (const auto &module : manifest.Value().modules)
                    if (!module.entry.empty())
                        Sign(package / module.entry);
                Sign(package / "resources/reference.lua");
                Write(
                    trust,
                    R"({"schemaVersion":1,"allowUnsigned":false,"publishers":[{"publisherId":"com.horo.tests","keyId":"host-test-key","algorithm":"ecdsa-p256-sha256","publicKeyHex":")" +
                        Hex(key.root.publicKey) + R"(","expiresAtUnixMilliseconds":0,"revoked":false}]})");
            }

            ~SignedPackage() {
                std::error_code error;
                std::filesystem::remove_all(root, error);
            }

            void Sign(const std::filesystem::path &path) {
                Write(path.string() + ".signature.json", Application::Internal::EncodeDetachedSignatureDocument(key.Sign(Read(path))));
            }

            Application::Internal::ExtensionScriptOptions Options() const {
                return {.packageRoot = package,
                        .hostTrust = trust,
                        .temporaryRoot = temporary,
                        .script = "resources/reference.lua",
                        .package = id,
                        .module = id + ".script",
                        .import = id + ".backend-import",
                        .publisher = key.root.publisherId};
            }

            void TrustPolicy(const std::uint64_t expiry, const bool revoked) {
                Write(
                    trust,
                    R"({"schemaVersion":1,"allowUnsigned":false,"publishers":[{"publisherId":"com.horo.tests","keyId":"host-test-key","algorithm":"ecdsa-p256-sha256","publicKeyHex":")" +
                        Hex(key.root.publicKey) + "\",\"expiresAtUnixMilliseconds\":" + std::to_string(expiry) +
                        ",\"revoked\":" + (revoked ? "true" : "false") + "}]}");
            }

            std::vector<std::string> Arguments() const {
                return {"extension",        "script-run",
                        "--extension-root", package.string(),
                        "--extension-id",   id,
                        "--module",         id + ".script",
                        "--import",         id + ".backend-import",
                        "--script",         "resources/reference.lua",
                        "--host-trust",     trust.string(),
                        "--publisher",      key.root.publisherId,
                        "--temporary-root", temporary.string(),
                        "--approve-native", "--approve-script",
                        "--output",         "json"};
            }

            ExternalProcessResult Run(std::vector<std::string> arguments, std::string &output) const {
                const auto *executable = std::getenv("HORO_REFERENCE_CLI");
                REQUIRE(executable);
                NativeExternalProcessRunner processes;
                auto result = processes.Run({.executable = executable,
                                             .arguments = std::move(arguments),
                                             .workingDirectory = root,
                                             .environment = {.set = {{"HOME", root.string()}, {"USERPROFILE", root.string()}}},
                                             .timeout = std::chrono::seconds{30},
                                             .onOutput =
                                                 [&output](ProcessOutputLine line) {
                    if (line.stream == ProcessOutputStream::StandardOutput) {
                        output += line.text;
                        output += '\n';
                    }
                },
                                             .maximumOutputBytes = 65536},
                                            {});
                REQUIRE(result.HasValue());
                if (result.Value().reason == ProcessTerminationReason::Exited && result.Value().exitCode == 0)
                    Horo::Tests::CliContract::ValidateEnvelope(nlohmann::json::parse(output));
                return result.Value();
            }
        };
    }  // namespace

    TEST_CASE("Actual CLI authenticates copied SDK modules and executes their real packaged Lua binding",
              "[extensions][sdk][cli][native]") {
        SignedPackage package;
        const auto foreign = package.temporary / ".horo-extension-script-0";
        REQUIRE(std::filesystem::create_directory(foreign));
        Write(foreign / "foreign.txt", "foreign-owned-by-fixture");
        std::string output;
        const auto result = package.Run(package.Arguments(), output);
        INFO(output);
        REQUIRE(result.exitCode == 0);
        CHECK(output.find("42") != std::string::npos);
        CHECK(Read(foreign / "foreign.txt").size() == std::string_view{"foreign-owned-by-fixture"}.size());
        CHECK_FALSE(std::filesystem::exists(package.temporary / ".horo-extension-script-1"));
    }

    TEST_CASE("Actual ordinary CLI remains admitted beside the exact native script descriptor", "[extensions][sdk][cli]") {
        SignedPackage package;
        std::string output;
        const auto result = package.Run({"host", "inspect", "--output", "json"}, output);
        INFO(output);
        REQUIRE(result.exitCode == 0);
        CHECK(output.find("horo.cli.host-inspection") != std::string::npos);
        CHECK(output.find(package.id) == std::string::npos);
        CHECK(std::filesystem::is_empty(package.temporary));
    }

    TEST_CASE("Actual CLI rejects unsigned mutations and missing consent before native package staging",
              "[extensions][sdk][cli][security]") {
        SignedPackage package;
        auto arguments = package.Arguments();
        SECTION("native consent absent") {
            const auto option = std::ranges::find(arguments, "--approve-native");
            REQUIRE(option != arguments.end());
            arguments.erase(option);
        }
        SECTION("packaged script changed after signature") {
            Write(package.package / "resources/reference.lua", "return 0");
        }
        SECTION("native binary changed after signature") {
            const auto bytes = Read(package.package / "extension.json");
            const auto manifest = ParseExtensionManifest({reinterpret_cast<const char *>(bytes.data()), bytes.size()});
            REQUIRE(manifest.HasValue());
            const auto path = package.package / manifest.Value().modules.front().entry;
            std::ofstream changed{path, std::ios::binary | std::ios::app};
            changed.put('x');
        }
        SECTION("publisher was revoked") {
            const auto bytes = Read(package.trust);
            std::string policy{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
            policy.replace(policy.find("\"revoked\":false"), std::string_view{"\"revoked\":false"}.size(), "\"revoked\":true");
            Write(package.trust, policy);
        }
        SECTION("publisher expired") {
            const auto bytes = Read(package.trust);
            std::string policy{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
            policy.replace(policy.find("\"expiresAtUnixMilliseconds\":0"), std::string_view{"\"expiresAtUnixMilliseconds\":0"}.size(),
                           "\"expiresAtUnixMilliseconds\":1");
            Write(package.trust, policy);
        }
        SECTION("manifest changed after signature") {
            std::ofstream output{package.package / "extension.json", std::ios::app};
            output << ' ';
        }
        SECTION("trust file is package-selected") {
            std::filesystem::copy_file(package.trust, package.package / "trust.json");
            const auto option = std::ranges::find(arguments, "--host-trust");
            REQUIRE(option != arguments.end());
            *(option + 1) = (package.package / "trust.json").string();
        }
        std::string output;
        CHECK(package.Run(std::move(arguments), output).exitCode != 0);
        INFO(output);
        CHECK(std::filesystem::is_empty(package.temporary));
    }

    TEST_CASE("Actual signed Lua remains subject to the product instruction and import capability bounds", "[extensions][sdk][cli][lua]") {
        SignedPackage package;
        SECTION("bounded instruction fuel") {
            Write(package.package / "resources/reference.lua", "while true do end");
        }
        SECTION("no ambient filesystem bindings") {
            Write(package.package / "resources/reference.lua", "return io.open('forbidden')");
        }
        package.Sign(package.package / "resources/reference.lua");
        std::string output;
        CHECK(package.Run(package.Arguments(), output).exitCode != 0);
        INFO(output);
        CHECK(std::filesystem::is_empty(package.temporary));
    }

    TEST_CASE("Current independent publisher policy is rechecked after authentic capture before staging and native admission",
              "[extensions][sdk][cli][security]") {
        using namespace Application::Internal;
        SignedPackage package;
        const auto options = package.Options();
        const auto captured = AuthenticateExtensionScript(options, {});
        INFO((captured.HasError() ? ErrorEvidence(captured.ErrorValue()) : "capture succeeded"));
        REQUIRE(captured.HasValue());
        SECTION("revoked after capture before staging") {
            package.TrustPolicy(0, true);
            CHECK(StageExtensionScript(options, *captured.Value()).HasError());
            CHECK(std::filesystem::is_empty(package.temporary));
        }
        SECTION("revoked after staging before actual native gate") {
            const auto staging = StageExtensionScript(options, *captured.Value());
            INFO((staging.HasError() ? ErrorEvidence(staging.ErrorValue()) : "staging succeeded"));
            REQUIRE(staging.HasValue());
            package.TrustPolicy(0, true);
            ExtensionScriptArtifactGate gate{staging.Value(), captured.Value()};
            const auto &module = captured.Value()->manifest.modules.front();
            CHECK(gate.Verify(staging.Value() / module.entry).HasError());
            CHECK(RevalidateExtensionScript(*captured.Value()).HasError());  // Same mandatory pre-Lua admission helper.
        }
        SECTION("publisher expires after capture without a policy-file change") {
            const auto expiry = std::chrono::system_clock::now() + std::chrono::seconds{5};
            package.TrustPolicy(static_cast<std::uint64_t>(
                                    std::chrono::duration_cast<std::chrono::milliseconds>(expiry.time_since_epoch()).count()),
                                false);
            const auto expiring = AuthenticateExtensionScript(options, {});
            INFO((expiring.HasError() ? ErrorEvidence(expiring.ErrorValue()) : "expiring capture succeeded"));
            REQUIRE(expiring.HasValue());
            std::this_thread::sleep_until(expiry);
            CHECK(StageExtensionScript(options, *expiring.Value()).HasError());
            CHECK(RevalidateExtensionScript(*expiring.Value()).HasError());
            CHECK(std::filesystem::is_empty(package.temporary));
        }
    }

}  // namespace Horo::Extensions::Tests
