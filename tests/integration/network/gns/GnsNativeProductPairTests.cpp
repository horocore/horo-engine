#include "GnsQualificationSupport.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Platform/ExternalProcess.h"
#include "Horo/Platform/SecureRandom.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <optional>
#include <thread>

namespace Horo::Network::NativeQualification {
    namespace {
        /** @brief Exclusively claimed directory; secrets are written only after the issuer verifies current owner and mode. */
        class FixtureDirectory final {
        public:
            void Create() {
                std::array<std::byte, 16> nonce{};
                const auto random = Platform::CreateNativeSecureRandomSource();
                REQUIRE(random);
                REQUIRE(random->Fill(nonce).HasValue());
                constexpr char digits[] = "0123456789abcdef";
                std::string token;
                for (const auto byte : nonce) {
                    const auto value = std::to_integer<unsigned>(byte);
                    token += digits[value >> 4];
                    token += digits[value & 15];
                }
                root = std::filesystem::temp_directory_path() / ("horo-native-secure-" + token);
                claimed_ = std::filesystem::create_directory(root);
                REQUIRE(claimed_);
                std::filesystem::permissions(root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
            }

            ~FixtureDirectory() {
                if (claimed_) {
                    std::error_code ignored;
                    std::filesystem::remove_all(root, ignored);
                }
            }

            std::filesystem::path root;

        private:
            bool claimed_{};
        };

        struct ProcessState final {
            std::atomic<bool> ready{};
            std::atomic<bool> qualified{};
            std::atomic<bool> rejected{};
            std::optional<Result<ExternalProcessResult>> result;
            std::string safeError;  // Written only by this worker; inspected only after join.
        };

        /** @brief Capture only fixed public phase tokens, never credential/key/certificate bytes or generic child output. */
        ExternalProcessRequest Request(const std::filesystem::path &directory, const unsigned side, ProcessState &state,
                                       const std::filesystem::path &nativeDirectory = {}, const bool rejectNativeRoot = false) {
            ExternalProcessRequest request;
            request.executable = HORO_GNS_PRODUCT_CHILD;
            request.arguments = {"--fixture-dir", directory.string(), "--side", std::to_string(side), "--reporter", "compact"};
            if (!nativeDirectory.empty()) {
                request.arguments.push_back("--native-dir");
                request.arguments.push_back(nativeDirectory.string());
            }
            if (rejectNativeRoot)
                request.arguments.push_back("--reject-native-root");
            request.timeout = std::chrono::seconds(22);
            request.gracefulTermination = std::chrono::milliseconds(200);
            request.maximumDrainDuration = std::chrono::milliseconds(500);
            request.maximumOutputBytes = 65536;
            request.maximumLineBytes = 4096;
            request.onOutput = [&state, side](const ProcessOutputLine line) {
                if ((line.text.starts_with("NATIVE_ERROR ") || line.text.starts_with("NATIVE_ASSERT ") ||
                     line.text.starts_with("NATIVE_STATE ") || line.text.starts_with("NATIVE_PHASE ") ||
                     line.text.starts_with("NATIVE_EVENTS") || line.text.starts_with("NATIVE_CODE ")) &&
                    state.safeError.size() < 1024)
                    state.safeError += line.text.substr(0, 256) + "; ";
                if (line.text == "NATIVE_READY server")
                    state.ready.store(true);
                if (line.text == (side == 0 ? "NATIVE_QUALIFIED server" : "NATIVE_QUALIFIED client"))
                    state.qualified.store(true);
                if (line.text == (side == 0 ? "NATIVE_REJECTED server" : "NATIVE_REJECTED client"))
                    state.rejected.store(true);
            };
            return request;
        }

        void RunChild(const ExternalProcessRequest &request, ProcessState &state, const CancellationToken &cancellation) noexcept {
            try {
                NativeExternalProcessRunner runner;
                state.result.emplace(runner.Run(request, cancellation));
            } catch (...) { /* Joined parent reports the missing terminal result; nothing escapes an owned worker. */
            }
        }

        void RequireQualified(const ProcessState &state, const bool expectNativeRejection) {
            INFO(state.safeError);
            REQUIRE(state.result.has_value());
            if (state.result->HasError())
                INFO(state.result->ErrorValue().domain.Value() << ':' << state.result->ErrorValue().code.Value());
            REQUIRE(state.result->HasValue());
            REQUIRE(state.result->Value().reason == ProcessTerminationReason::Exited);
            REQUIRE(state.result->Value().exitCode == 0);
            REQUIRE(state.qualified.load() == !expectNativeRejection);
            REQUIRE(state.rejected.load() == expectNativeRejection);
        }

        /** @brief Keep one shared Horo credential directory while optionally selecting a different native CA for the client. */
        void RunPair(const std::filesystem::path &credentials, const std::filesystem::path &clientNative = {},
                     const bool expectNativeRejection = false) {
            ProcessState server;
            ProcessState client;
            CancellationSource cancellation;
            const auto serverRequest = Request(credentials, 0, server, {}, expectNativeRejection);
            const auto clientRequest = Request(credentials, 1, client, clientNative, expectNativeRejection);
            std::jthread serverWorker([&] {
                RunChild(serverRequest, server, cancellation.Token());
            });
            const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!server.ready.load() && std::chrono::steady_clock::now() < readyDeadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (server.ready.load()) {
                std::jthread clientWorker([&] {
                    RunChild(clientRequest, client, cancellation.Token());
                });
                clientWorker.join();  // Both child trees retire before the private fixture directory can be removed.
                std::ofstream stop(credentials / "stop-server");  // Public teardown signal, never an admission authority.
                stop << "stop\n";
            } else {
                cancellation.RequestCancellation();
            }
            serverWorker.join();
            INFO("server public diagnostics: " << server.safeError);
            INFO("client public diagnostics: " << client.safeError);
            REQUIRE(server.ready.load());
            RequireQualified(client, expectNativeRejection);
            RequireQualified(server, expectNativeRejection);
        }

        void Provision(FixtureDirectory &fixture) {
            fixture.Create();
            const auto provisioned = CreateEphemeralFixture(fixture.root);
            if (provisioned.HasError())
                INFO(provisioned.ErrorValue().domain.Value() << ':' << provisioned.ErrorValue().code.Value());
            REQUIRE(provisioned.HasValue());
        }

        std::array<char, 32> PublicRoot(const std::filesystem::path &directory) {
            std::array<char, 32> root{};
            std::ifstream file(directory / "root.bin", std::ios::binary);
            file.read(root.data(), root.size());
            REQUIRE(file.gcount() == static_cast<std::streamsize>(root.size()));
            REQUIRE(file.peek() == std::char_traits<char>::eof());
            return root;
        }
    }  // namespace

    TEST_CASE("Native qualification credential reads reject empty truncated and oversized owner-only files",
              "[integration][network][gns][native-fixture]") {
        FixtureDirectory fixture;
        Provision(fixture);
        REQUIRE(LoadCredentialProviders(fixture.root, 0).HasValue());
        std::uintmax_t bytes{};
        SECTION("Empty file") {}
        SECTION("Truncated credential") {
            bytes = 31;
        }
        SECTION("Lookahead detects an extra byte") {
            bytes = 33;
        }
        std::filesystem::resize_file(fixture.root / "credential.bin", bytes);
        const auto rejected = LoadCredentialProviders(fixture.root, 0);
        REQUIRE(rejected.HasError());
        REQUIRE(rejected.ErrorValue().domain.Value() == NetworkErrors::AuthenticationTrustUnavailable.domain.Value());
        REQUIRE(rejected.ErrorValue().code.Value() == NetworkErrors::AuthenticationTrustUnavailable.code.Value());
    }

    TEST_CASE("Two native ProductHost processes authenticate through real PKI and dispatch registered gameplay",
              "[integration][network][gns][native-pair]") {
        FixtureDirectory fixture;
        Provision(fixture);
        RunPair(fixture.root);
    }

    TEST_CASE("Distinct native CA roots reject both ProductHost peers before readiness despite a shared Horo proof authority",
              "[integration][network][gns][native-pair][native-rejection]") {
        FixtureDirectory server;
        FixtureDirectory client;
        Provision(server);
        Provision(client);
        REQUIRE(PublicRoot(server.root) != PublicRoot(client.root));
        // Both LoadCredentialProviders calls still read server/credential.bin. Only native trust/issuance differs.
        // Thus credential incompatibility cannot mask the native CA rejection, and no secret is copied into arguments or logs.
        RunPair(server.root, client.root, true);
    }
}  // namespace Horo::Network::NativeQualification
