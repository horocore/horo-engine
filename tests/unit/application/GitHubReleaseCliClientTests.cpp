#include "Horo/Release/GitHubReleaseCliClient.h"
#include "ReleaseTestFixtures.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

using namespace Horo;
using namespace Horo::Release;
using namespace ReleaseTestFixtures;

namespace {
    class TemporaryFile final {
    public:
        TemporaryFile()
            : path(std::filesystem::temp_directory_path() /
                   ("horo-cli-fixture-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::ofstream output(path, std::ios::binary);
            output << "payload";
        }

        ~TemporaryFile() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }

        std::filesystem::path path;
    };

    class FakeGitHubProcess final : public IExternalProcessRunner {
    public:
        [[nodiscard]] Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &) override {
            REQUIRE(request.executable == "gh");
            CHECK(request.environment.set.empty());
            for (const auto &argument : request.arguments)
                CHECK(argument.find("secret-token") == std::string::npos);
            const auto &arguments = request.arguments;
            std::string output;
            int exitCode = 0;
            if (!authenticated) {
                exitCode = 1;
            } else if (arguments[0] == "api") {
                const auto &endpoint = arguments[1] == "--method" ? arguments[3] : arguments[1];
                if (endpoint.find("/releases/tags/") != std::string::npos) {
                    output = R"({"id":72,"tag_name":"v0.4.2","draft":false})";
                } else if (endpoint.find("/assets?") != std::string::npos) {
                    output = "[";
                    bool first = true;
                    for (const auto &[name, bytes] : assets) {
                        (void)bytes;
                        if (!first)
                            output += ",";
                        output += "{\"id\":1,\"name\":\"" + name + "\"}";
                        first = false;
                    }
                    output += "]";
                } else if (endpoint.ends_with("/releases/latest")) {
                    output = "{\"id\":" + std::to_string(latestReleaseId) + "}";
                } else if (arguments[1] == "--method") {
                    ++commits;
                    latestReleaseId = 72U;
                } else {
                    exitCode = 1;
                }
            } else if (arguments.size() >= 3U && arguments[0] == "release" && arguments[1] == "upload") {
                const auto file = std::filesystem::path(arguments[3]);
                std::ifstream input(file, std::ios::binary);
                assets[file.filename().string()] = std::string{std::istreambuf_iterator<char>{input}, {}};
                ++uploads;
                if (interruptUpload)
                    exitCode = 1;
            } else if (arguments.size() >= 3U && arguments[0] == "release" && arguments[1] == "download") {
                const auto name = arguments[6];
                const auto destination = arguments[8];
                if (!assets.contains(name)) {
                    exitCode = 1;
                } else {
                    std::ofstream file(destination, std::ios::binary);
                    file << assets.at(name);
                }
            } else {
                exitCode = 1;
            }
            if (!output.empty() && request.onOutput)
                request.onOutput({ProcessOutputStream::StandardOutput, output, false});
            return Result<ExternalProcessResult>::Success({ProcessTerminationReason::Exited, exitCode, ProcessStopCause::None});
        }

        std::map<std::string, std::string> assets;
        std::uint64_t latestReleaseId{5U};
        int uploads{};
        int commits{};
        bool authenticated{true};
        bool interruptUpload{};
    };
}  // namespace

TEST_CASE("GitHub CLI client verifies uploaded bytes and treats matching retry as idempotent", "[release][github]") {
    TemporaryFile file;
    FakeGitHubProcess processes;
    GitHubReleaseCliClient client{processes};
    auto release = client.FindExisting("horocore/horo-engine", "v0.4.2");
    REQUIRE(release.HasValue());
    const ReleaseArtifactRecord artifact{"bin/editor", ReleaseArtifactRole::Binary, 7U, Digest("payload")};

    REQUIRE(client.UploadExact(release.Value(), "bin%2Feditor", file.path, artifact).HasValue());
    CHECK(processes.uploads == 1);
    REQUIRE(client.UploadExact(release.Value(), "bin%2Feditor", file.path, artifact).HasValue());
    CHECK(processes.uploads == 1);
    auto measured = client.ReadAsset(release.Value(), "bin%2Feditor");
    REQUIRE(measured.HasValue());
    CHECK(measured.Value().size == artifact.size);
    CHECK(measured.Value().digest == artifact.digest);

    processes.assets["bin%2Feditor"] = "changed";
    CHECK(client.UploadExact(release.Value(), "bin%2Feditor", file.path, artifact).HasError());
    CHECK(processes.uploads == 1);
}

TEST_CASE("GitHub CLI client fails closed on missing authorization and verifies stable commit", "[release][github]") {
    FakeGitHubProcess processes;
    GitHubReleaseCliClient client{processes};
    processes.authenticated = false;
    CHECK(client.FindExisting("horocore/horo-engine", "v0.4.2").HasError());
    CHECK(processes.uploads == 0);

    processes.authenticated = true;
    auto release = client.FindExisting("horocore/horo-engine", "v0.4.2");
    REQUIRE(release.HasValue());
    processes.assets["manifest.json"] = "manifest";
    CHECK(client.Commit(release.Value(), {ReleaseChannelKind::Preview, {}}, Digest("manifest")).HasError());
    CHECK(processes.commits == 0);
    REQUIRE(client.Commit(release.Value(), {ReleaseChannelKind::Stable, {}}, Digest("manifest")).HasValue());
    CHECK(processes.commits == 1);
    CHECK(processes.latestReleaseId == release.Value().releaseId);
    CHECK(client.Commit(release.Value(), {ReleaseChannelKind::Stable, {}}, Digest("wrong")).HasError());
    CHECK(processes.commits == 1);
}

TEST_CASE("GitHub CLI client resolves an interrupted upload only after remote verification", "[release][github]") {
    TemporaryFile file;
    FakeGitHubProcess processes;
    processes.interruptUpload = true;
    GitHubReleaseCliClient client{processes};
    auto release = client.FindExisting("horocore/horo-engine", "v0.4.2");
    REQUIRE(release.HasValue());
    const ReleaseArtifactRecord artifact{"bin/editor", ReleaseArtifactRole::Binary, 7U, Digest("payload")};
    CHECK(client.UploadExact(release.Value(), "bin%2Feditor", file.path, artifact).HasValue());
    CHECK(processes.uploads == 1);
}
