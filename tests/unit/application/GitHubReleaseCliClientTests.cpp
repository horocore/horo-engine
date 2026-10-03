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
        explicit FakeGitHubProcess(const bool authorized = true, const bool retargeted = false)
            : authenticated(authorized), changedTagTarget(retargeted) {}

        [[nodiscard]] Result<ExternalProcessResult> Run(const ExternalProcessRequest &request, const CancellationToken &) override {
            REQUIRE(request.executable == "gh");
            CHECK(request.environment.set.empty());
            for (const auto &argument : request.arguments)
                CHECK(argument.find("secret-token") == std::string::npos);
            std::string output;
            const int exitCode = !authenticated                  ? 1
                                 : request.arguments[0] == "api" ? HandleApi(request.arguments, output)
                                                                 : HandleRelease(request.arguments);
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
        bool changedTagTarget{};

    private:
        int HandleApi(const std::vector<std::string> &arguments, std::string &output) {
            const auto &endpoint = arguments[1] == "--method" ? arguments[3] : arguments[1];
            if (endpoint.find("/releases/tags/") != std::string::npos) {
                output = R"({"id":72,"tag_name":"v0.4.2","draft":false,"body":"Reviewed notes"})";
            } else if (endpoint.find("/git/ref/tags/") != std::string::npos) {
                output = R"({"object":{"type":"tag","sha":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}})";
            } else if (endpoint.find("/git/tags/") != std::string::npos) {
                output = changedTagTarget ? R"({"object":{"type":"commit","sha":"cccccccccccccccccccccccccccccccccccccccc"}})"
                                          : R"({"object":{"type":"commit","sha":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})";
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
                return 1;
            }
            return 0;
        }

        int HandleRelease(const std::vector<std::string> &arguments) {
            if (arguments.size() < 3U || arguments[0] != "release")
                return 1;
            if (arguments[1] == "upload") {
                const auto file = std::filesystem::path(arguments[3]);
                std::ifstream input(file, std::ios::binary);
                assets[file.filename().string()] = std::string{std::istreambuf_iterator<char>{input}, {}};
                ++uploads;
                return interruptUpload ? 1 : 0;
            }
            if (arguments[1] == "download") {
                const auto &name = arguments[6];
                if (!assets.contains(name))
                    return 1;
                std::ofstream file(arguments[8], std::ios::binary);
                file << assets.at(name);
                return 0;
            }
            return 1;
        }
    };
}  // namespace

TEST_CASE("GitHub CLI client verifies uploaded bytes and treats matching retry as idempotent", "[release][github]") {
    TemporaryFile file;
    FakeGitHubProcess processes;
    GitHubReleaseCliClient client{processes};
    auto release = client.FindExisting("horocore/horo-engine", "v0.4.2");
    REQUIRE(release.HasValue());
    CHECK(release.Value().sourceCommit == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
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
    FakeGitHubProcess retargeted{true, true};
    GitHubReleaseCliClient changedClient{retargeted};
    CHECK(changedClient.ReadAsset(release.Value(), "bin%2Feditor").HasError());
}

TEST_CASE("GitHub CLI client fails closed on missing authorization and verifies stable commit", "[release][github]") {
    FakeGitHubProcess processes{false};
    GitHubReleaseCliClient client{processes};
    CHECK(client.FindExisting("horocore/horo-engine", "v0.4.2").HasError());
    CHECK(processes.uploads == 0);

    FakeGitHubProcess authorized;
    GitHubReleaseCliClient ready{authorized};
    auto release = ready.FindExisting("horocore/horo-engine", "v0.4.2");
    REQUIRE(release.HasValue());
    CHECK(release.Value().body == "Reviewed notes");
    authorized.assets["manifest.json"] = "manifest";
    CHECK(ready.Commit(release.Value(), {ReleaseChannelKind::Preview, {}}, Digest("manifest")).HasError());
    CHECK(authorized.commits == 0);
    REQUIRE(ready.Commit(release.Value(), {ReleaseChannelKind::Stable, {}}, Digest("manifest")).HasValue());
    CHECK(authorized.commits == 1);
    CHECK(authorized.latestReleaseId == release.Value().releaseId);
    CHECK(ready.Commit(release.Value(), {ReleaseChannelKind::Stable, {}}, Digest("wrong")).HasError());
    CHECK(authorized.commits == 1);
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
