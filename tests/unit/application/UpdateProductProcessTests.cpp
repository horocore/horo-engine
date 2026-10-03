#include "Horo/Platform/ExternalProcess.h"
#include "UpdateActivationTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace Horo::Release;

namespace {
    class ProbeDirectory final {
    public:
        ProbeDirectory()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-update-probe-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(root / "versions");
        }

        ~ProbeDirectory() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path root;
    };

    class CapturingProcess final : public Horo::IExternalProcessRunner {
    public:
        [[nodiscard]] Horo::Result<Horo::ExternalProcessResult> Run(const Horo::ExternalProcessRequest &request,
                                                                    const Horo::CancellationToken &) override {
            ++runs;
            executable = request.executable;
            workingDirectory = request.workingDirectory;
            arguments = request.arguments;
            environment = request.environment.base;
            timeout = request.timeout;
            return Horo::Result<Horo::ExternalProcessResult>::Success(result);
        }

        std::size_t runs{};
        std::string executable;
        std::filesystem::path workingDirectory;
        std::vector<std::string> arguments;
        Horo::ProcessEnvironmentBase environment{Horo::ProcessEnvironmentBase::InheritWithOverrides};
        std::chrono::milliseconds timeout{};
        Horo::ExternalProcessResult result{};
    };
}  // namespace

TEST_CASE("Authenticated product startup uses the signed entrypoint with a bounded shell-free runner", "[release][update][probe]") {
    ProbeDirectory install;
    Horo::NativeDurableFileSystem files;
    auto verifier = TestSupport::MakeVerifier();
    auto version = TestSupport::MakeVersion(install.root, files, verifier);
    CapturingProcess processes;
    constexpr UpdateArchiveLimits limits{.maximumEntries = 4U, .maximumFileBytes = 1024U, .maximumExpandedBytes = 1024U};
    const std::array arguments{std::string{"--self-test"}};
    REQUIRE(ProbeVerifiedUpdateEntrypoint(version, limits, verifier, processes, arguments, std::chrono::seconds{2}).HasValue());
    CHECK(processes.runs == 1U);
    CHECK(processes.executable == (version.stageRoot / "bin/editor").string());
    CHECK(processes.workingDirectory == version.stageRoot);
    CHECK(processes.arguments == std::vector<std::string>{"--self-test"});
    CHECK(processes.environment == Horo::ProcessEnvironmentBase::Replace);
    CHECK(processes.timeout == std::chrono::seconds{2});

    processes.result.exitCode = 7;
    CHECK(ProbeVerifiedUpdateEntrypoint(version, limits, verifier, processes, arguments, std::chrono::seconds{2}).HasError());
#if !defined(_WIN32)
    processes.result.exitCode = 0;
    std::filesystem::permissions(version.stageRoot / "bin/editor", std::filesystem::perms{0644});
    CHECK(ProbeVerifiedUpdateEntrypoint(version, limits, verifier, processes, arguments, std::chrono::seconds{2}).HasError());
    std::filesystem::permissions(version.stageRoot / "bin/editor", std::filesystem::perms{0755});
#endif
    version.inventory.front().path = "bin/other";
    CHECK(ProbeVerifiedUpdateEntrypoint(version, limits, verifier, processes, arguments, std::chrono::seconds{2}).HasError());
    CHECK(processes.runs == 2U);
}
